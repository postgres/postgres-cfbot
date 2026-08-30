-- Configure parameters to avoid competing plans.

-- Prevent seqscans or bitmapscans that could be combined
-- with a sort as an alternative to an index scan that
-- satisfies the query order.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
-- Every query should rely on an index, and a scan without
-- a sort will always be cheaper than a scan with a sort.
SET enable_indexscan = on;
SET enable_indexonlyscan = on;
-- Disable hashagg that could provide an alterntative to a
-- GROUP BY that doesn't require the input to be sorted.
SET enable_hashagg = off;

CREATE SCHEMA slope;
SET search_path TO slope;

--
-- Merge joins
--
CREATE TABLE t (a int PRIMARY KEY);
INSERT INTO t SELECT generate_series(1, 5);
ANALYZE t;
CREATE TABLE u (a int PRIMARY KEY);
INSERT INTO u SELECT generate_series(1, 5);
ANALYZE u;

SET enable_hashjoin = off;
SET enable_nestloop = off;

SET enable_slope = off;
EXPLAIN (COSTS OFF) SELECT * FROM t JOIN u USING (a);
SET enable_slope = on;
EXPLAIN (COSTS OFF) SELECT * FROM t JOIN u USING (a);



--
-- Default vs custom btree opfamilies.
--

-- SLOPE only trusts each type's default opfamily on both the index and the
-- query pathkey.  Cross-product:
--   (1) index default, query custom  -> not supported
--   (2) index custom,  query default -> not supported
--   (3) index custom,  query custom  -> not supported (even when they match)
--   (4) index default, query default -> supported
--
-- <# … ># are clones of int4 comparison, registered only in test_int4_ops,
-- so ORDER BY … USING <# forces a non-default pathkey opfamily.
--
CREATE OPERATOR FAMILY test_int4_ops USING btree;

CREATE OPERATOR <# (
  LEFTARG = int4, RIGHTARG = int4,
  PROCEDURE = int4lt, COMMUTATOR = >#, NEGATOR = >=#
);
CREATE OPERATOR ># (
  LEFTARG = int4, RIGHTARG = int4,
  PROCEDURE = int4gt, COMMUTATOR = <#, NEGATOR = <=#
);
CREATE OPERATOR <=# (
  LEFTARG = int4, RIGHTARG = int4,
  PROCEDURE = int4le, COMMUTATOR = >=#, NEGATOR = >#
);
CREATE OPERATOR >=# (
  LEFTARG = int4, RIGHTARG = int4,
  PROCEDURE = int4ge, COMMUTATOR = <=#, NEGATOR = <#
);

CREATE OPERATOR CLASS test_int4_ops FOR TYPE int4
  USING btree FAMILY test_int4_ops AS
  OPERATOR 1 <#,
  OPERATOR 2 <=#,
  OPERATOR 3 =,
  OPERATOR 4 >=#,
  OPERATOR 5 >#,
  FUNCTION 1 btint4cmp(int4, int4);

CREATE TABLE t_op (x_def int4, x_cust int4);
INSERT INTO t_op SELECT i, i FROM generate_series(1, 5) i;
CREATE INDEX t_op_x_def ON t_op (x_def);
CREATE INDEX t_op_x_cust ON t_op (x_cust test_int4_ops);
ANALYZE t_op;

-- (4) index default, query default
EXPLAIN (COSTS OFF)
SELECT x_def + 1 FROM t_op ORDER BY 1;

-- (1) index default, query custom
EXPLAIN (COSTS OFF)
SELECT x_def + 1 FROM t_op ORDER BY 1 USING <#;

-- (2) index custom, query default
EXPLAIN (COSTS OFF)
SELECT x_cust + 1 FROM t_op ORDER BY 1;

-- (3) index custom, query custom
EXPLAIN (COSTS OFF)
SELECT x_cust + 1 FROM t_op ORDER BY 1 USING <#;

--
-- SLOPE (Scalar function Leveraging Ordered Path Evaluation)
-- Test that monotonic functions can use indexes for ordering
--

-- Create test table with various data types
CREATE TABLE src (
    id serial PRIMARY KEY,
    v_int2 int2,
    v_int4 int4,
    v_int8 int8,
    v_float4 float4,
    v_float8 float8,
    v_numeric numeric,
    ts timestamp,
    tstz timestamptz
);

-- Insert some test data
INSERT INTO src (v_int2, v_int4, v_int8, v_float4, v_float8, v_numeric, ts, tstz)
SELECT
    (i % 100)::int2,
    i,
    i::int8,
    i::float4,
    i::float8,
    i::numeric,
    '2020-01-01'::timestamp + (i || ' hours')::interval,
    '2020-01-01'::timestamptz + (i || ' hours')::interval
FROM generate_series(1, 1000) i;

-- Create indexes on the columns we'll test
CREATE INDEX src_v_int4_idx ON src (v_int4);
CREATE INDEX src_v_int8_idx ON src (v_int8);
CREATE INDEX src_v_float8_idx ON src (v_float8);
CREATE INDEX src_v_numeric_idx ON src (v_numeric);
CREATE INDEX src_ts_idx ON src (ts);
CREATE INDEX src_tstz_idx ON src (tstz);

-- Analyze to get good statistics
ANALYZE src;

--
-- Test that SLOPE is enabled and can be disabled
--

SET enable_slope = off;
-- Basic: floor(float8) should sort even though the index order
-- produces the result in the correct order.
explain (costs off, verbose)
select floor(v_float8), count(*) from src group by 1;
RESET enable_slope;
SHOW enable_slope;

--
-- Test GROUP BY with monotonic function
--

-- Basic: floor(float8) should use index on v_float8
explain (costs off, verbose)
select floor(v_float8), count(*) from src group by 1;

-- ceil(float8) should use index on v_float8
explain (costs off, verbose)
select ceil(v_float8), count(*) from src group by 1;

-- floor(numeric) should use index on v_numeric
explain (costs off, verbose)
select floor(v_numeric), count(*) from src group by 1;

-- timestamp::date cast should use index on ts
explain (costs off, verbose)
select ts::date, count(*) from src group by 1;

-- date_trunc on timestamp should use index
explain (costs off, verbose)
select date_trunc('day', ts), count(*) from src group by 1;


--
-- Test arithmetic operations
--

-- Addition: v_int4 + 10 is increasing in v_int4
explain (costs off, verbose)
select v_int4 + 10, count(*) from src group by 1;

-- Subtraction: v_int4 - 10 is increasing in v_int4
explain (costs off, verbose)
select v_int4 - 10, count(*) from src group by 1;

-- Multiplication by positive constant: v_int4 * 2 is increasing
explain (costs off, verbose)
select v_int4 * 2, count(*) from src group by 1;

-- Division by positive constant: v_int4 / 2 is increasing
explain (costs off, verbose)
select v_int4 / 2, count(*) from src group by 1;

--
-- Test decreasing functions
-- These queries can't use the index order because group pathkeys
-- are always ASC NULLS LAST, while -int_v4 is DESC NULLS LAST.

-- Unary minus: -v_int4 is decreasing in v_int4
explain (costs off, verbose)
select -v_int4, count(*) from src group by 1;

-- Subtraction from constant: 1000 - v_int4 is decreasing in v_int4
explain (costs off, verbose)
select 1000 - v_int4, count(*) from src group by 1;

-- Multiplication by negative constant: v_int4 * (-2) is decreasing
explain (costs off, verbose)
select v_int4 * (-2), count(*) from src group by 1;

-- Division by negative constant: v_int4 / (-2) is decreasing
explain (costs off, verbose)
select v_int4 / (-2), count(*) from src group by 1;

--
-- Test ORDER BY with monotonic function
--

-- ORDER BY floor(v_float8) should use index
explain (costs off, verbose)
select floor(v_float8), v_float8 from src order by 1;

-- ORDER BY -v_int4 DESC requires sorting
-- the index order is v_int4 ASC and, implicitly, NULLS LAST
-- a forward scan gives -v_int4 DESC NULLS LAST
-- a backward scan gives -v_int4 ASC NULLS FIRST
-- the query order is DESC and, implicitly, NULLS FIRST
explain (costs off, verbose)
select -v_int4 from src order by 1 desc;

-- ORDER BY -v_int4 ASC requires sorting
-- a forward scan gives -v_int4 DESC NULLS LAST
-- a backward scan gives -v_int4 ASC NULLS FIRST
-- the query order is -v_int4 ASC NULLS LAST
explain (costs off, verbose)
select -v_int4 from src order by 1;

--
-- Test nested monotonic function
--

-- floor(floor(x)) should still use index
explain (costs off, verbose)
select floor(floor(v_float8)), count(*) from src group by 1;

-- floor(v + 1) should use index
explain (costs off, verbose)
select floor(v_float8 + 1), count(*) from src group by 1;

--
-- Test all index/query direction+nulls combinations for SLOPE.
-- For an increasing function like floor(), the scan uses the index when both
-- direction and nulls agree (Forward) or both are flipped (Backward).
-- If one agrees and the other disagrees, then the scan cannot use the
-- index and a sort is required.
--
CREATE TABLE nulls_tmp (v float8);
INSERT INTO nulls_tmp VALUES (1), (NULL), (2);
ANALYZE nulls_tmp;

CREATE TEMPORARY TABLE nulls_results (
    seq serial,
    sign text, index_order text, query_order text, scan_method text,
    example text
);
DO $$
DECLARE
    r record;
    plan_json json;
    node_type text;
    query text;
    r1 text;
    r2 text;
BEGIN
    FOR r IN
        SELECT idx_dir, idx_nf, qry_dir, qry_nf, sign
        FROM unnest(ARRAY['ASC','DESC'])   WITH ORDINALITY AS idx_dir(idx_dir, idx_dir_i),
             unnest(ARRAY['FIRST','LAST']) WITH ORDINALITY AS idx_nf (idx_nf,  idx_nf_i),
             unnest(ARRAY['+','-'])        WITH ORDINALITY AS sign   (sign,    sign_i),
             unnest(ARRAY['FIRST','LAST']) WITH ORDINALITY AS qry_nf (qry_nf,  qry_nf_i),
             unnest(ARRAY['ASC','DESC'])   WITH ORDINALITY AS qry_dir(qry_dir, qry_dir_i)
        ORDER BY qry_dir_i, qry_nf_i, sign_i, idx_dir_i, idx_nf_i
    LOOP
        EXECUTE format('CREATE INDEX nulls_tmp_idx ON nulls_tmp (v %s NULLS %s)',
                        r.idx_dir, r.idx_nf);
        query := format('SELECT floor(0.5 %s v) as x FROM nulls_tmp ORDER BY 1 %s NULLS %s',
            r.sign, r.qry_dir, r.qry_nf);
        EXECUTE format('EXPLAIN (FORMAT JSON) %s', query)
        INTO plan_json;

        SET enable_slope = off;
        EXECUTE 'SELECT string_agg(coalesce(x::text, ''NULL''), '','') FROM (' || query || ') tmp(x)' into r1;
        SET enable_slope = on;
        EXECUTE 'SELECT string_agg(coalesce(x::text, ''NULL''), '','') FROM (' || query || ') tmp(x)' into r2;
        if r1 <> r2 then
            raise notice 'query %', query;
            raise exception ' r1=%, r2=%', r1, r2;
        end if;
        node_type := plan_json->0->'Plan'->>'Node Type';
        INSERT INTO nulls_results (sign, index_order, query_order, scan_method, example) VALUES (
            r.sign,
            r.idx_dir || ' NULLS ' || r.idx_nf,
            r.qry_dir || ' NULLS ' || r.qry_nf,
            CASE WHEN node_type IN ('Index Only Scan', 'Index Scan')
                 THEN plan_json->0->'Plan'->>'Scan Direction'
                 ELSE node_type
            END,
            r2
        );
        EXECUTE format('DROP INDEX nulls_tmp_idx');
    END LOOP;
END;
$$;

SELECT sign, index_order, query_order, scan_method, example
FROM nulls_results ORDER BY seq;

--
-- Test special numeric values
--
-- This check the correctness of the query for a nullable column
-- also containing special values (-inf, +inf and NaN)
-- represented as float8, float4, numeric and user defined
-- domains with float8 base type.
CREATE DOMAIN non42 AS float8 CHECK (value != 42);
CREATE DOMAIN fp_real AS float8;
CREATE UNLOGGED TABLE numeric_corners (i serial, x float8);
INSERT INTO numeric_corners (x)
SELECT x::float8 as x FROM
        unnest(ARRAY['-inf', '-1', '0', '3', 'inf', 'nan', NULL]) x(x)
ORDER BY 1;

CREATE INDEX numeric_corners_xfp8_idx_nulllast ON numeric_corners ((x::float8) ASC NULLS LAST);
CREATE INDEX numeric_corners_xfp8_idx_nullfirst ON numeric_corners ((x::float8) ASC NULLS FIRST);
CREATE INDEX numeric_corners_xfp4_idx_nulllast ON numeric_corners ((x::float4) ASC NULLS LAST);
CREATE INDEX numeric_corners_xfp4_idx_nullfirst ON numeric_corners ((x::float4) ASC NULLS FIRST);
CREATE INDEX numeric_corners_xnum_idx_nulllast ON numeric_corners ((x::numeric) ASC NULLS LAST);
CREATE INDEX numeric_corners_xnum_idx_nullfirst ON numeric_corners ((x::numeric) ASC NULLS FIRST);
CREATE INDEX numeric_corners_xnon42_idx_nulllast ON numeric_corners ((x::non42) ASC NULLS LAST);
CREATE INDEX numeric_corners_xnon42_idx_nullfirst ON numeric_corners ((x::non42) ASC NULLS FIRST);
CREATE INDEX numeric_corners_xfpreal_idx_nulllast ON numeric_corners ((x::fp_real) ASC NULLS LAST);
CREATE INDEX numeric_corners_xfpreal_idx_nullfirst ON numeric_corners ((x::fp_real) ASC NULLS FIRST);

CREATE TEMPORARY TABLE numeric_corners_results (
    seq serial,
    expr text COLLATE "C",
    sort_order text COLLATE "C",
    nulls_order text COLLATE "C",
    typename text COLLATE "C",
    result float8[],
    expected float8[],
    expected_seq int4[],
    nan_values float8[],
    plan1_json json,
    plan2_json json
);
DO $$
DECLARE
    r record;
    result float8[];
    expected float8[];
    expected_seq int4[];
    nan_values float8[];
    query text;
    agg_query text;
    plan_query text;
    nan_query text;
    expected_seq_query text;
    plan1_json json;
    plan2_json json;
BEGIN

    FOR r IN
        SELECT replace(replace(s, 'x', 'x::' || t), 'A', a || '::' || t) as expr, sort_order, nulls_order, t as typename
        FROM
        unnest(ARRAY['ASC', 'DESC']) WITH ORDINALITY AS so(sort_order, so_i),
        unnest(ARRAY['FIRST', 'LAST']) WITH ORDINALITY AS nf(nulls_order, no_i),
        unnest(ARRAY['float8', 'float4', 'numeric', 'non42', 'fp_real']) WITH ORDINALITY AS t(t, t_i),
        unnest(ARRAY[
        'x+A', 'x-A', 'x*A', 'x/A',
        'A+x', 'A-x', 'A*x', '-x'
        ]) WITH ORDINALITY AS s(s, s_i),
        unnest(ARRAY['''inf''', '''-inf''', '1']) WITH ORDINALITY AS a(a, a_i)
        ORDER BY s_i, so_i, no_i, t_i
    LOOP
        query := 'SELECT *, (' || r.expr || ') as f '
                 || 'FROM numeric_corners '
                 || 'ORDER BY f ' || r.sort_order || ' NULLS ' || r.nulls_order;
        nan_query := 'SELECT array_agg(x) FROM numeric_corners
        WHERE ' || r.expr || ' = ''nan''::' || r.typename || ' AND x != ''nan''::' || r.typename;
        agg_query := 'SELECT array_agg(f::float8) FROM (' || query || ') tmp';
        expected_seq_query := 'SELECT array_agg(i::int4) FROM (' || query || ') tmp';
        plan_query := 'EXPLAIN (FORMAT JSON) ' || query;
        -- slope optimization disabled
        SET enable_slope = off;
        EXECUTE agg_query into result;
        EXECUTE plan_query into plan2_json;
        -- slope optimization enabled
        SET enable_slope = on;
        EXECUTE agg_query into expected;
        EXECUTE expected_seq_query into expected_seq;
        EXECUTE nan_query into nan_values;
        EXECUTE plan_query into plan1_json;
        -- store results
        INSERT INTO numeric_corners_results
        (expr, sort_order, nulls_order, typename, result, expected, expected_seq, nan_values, plan1_json, plan2_json)
        VALUES (r.expr, r.sort_order, r.nulls_order, r.typename, result, expected, expected_seq, nan_values, plan1_json, plan2_json);
    END LOOP;
END;
$$;

-- display failing test cases
SELECT seq, expr
, sort_order, nulls_order
, expected, expected_seq, result, nan_values
, plan2_json->0->'Plan'->>'Node Type' as plan2
FROM numeric_corners_results
WHERE expected != result;

-- check the number of test cases
SELECT expected = result as passed, count(1)
FROM numeric_corners_results
GROUP BY 1;

--
-- Test pathkey chains: f(x), x (descending chain)
-- The order of f(x) is implied by the order of x when f is monotonic.
--

explain (costs off, verbose)
select * from src order by ts::date, ts;

--
-- Test pathkey chains: x, f(x) (ascending chain)
-- f(x) is constant when x is constant, so f(x) is a redundant tiebreaker.
--
-- When btree equality on x implies image equality (equalimage), any
-- deterministic f(x) is constant on each index group, so f(x) is a
-- redundant tiebreaker and the index on x alone can satisfy ORDER BY x, f(x).
-- When equalimage does not hold, values that compare equal under the index
-- can still differ in f(x); the planner must keep sorting on f(x),
-- unless f(x) is monotonic
--

-- int4 has btequalimage; numeric/float8 do not (e.g. 1.0 = 1.00, -0.0 = 0.0).

explain (costs off, verbose)
select * from src order by ts, ts::date;

-- non-monotonic f(x) after pinned x with equalimage
-- can safely ignore f(x) tie breaker and use index order alone
explain (costs off, verbose)
select v_int4, abs(v_int4)
from src order by v_int4, abs(v_int4);

-- non-monotonic f(x) before x: cannot use index order alone
explain (costs off, verbose)
select mod(v_int4, 7), v_int4
from src order by mod(v_int4, 7), v_int4;

-- float_ops has no equalimage so it requires incremental sort
explain (costs off, verbose)
select * from src order by v_float8, cos(v_float8);

-- monotonic f without equalimage: index order on n implies order on 2 * n
explain (costs off, verbose)
select * from src order by v_float8, 2 * v_float8;

--
-- INCLUDE columns are not index sort keys; build_index_pathkeys must stop
-- at nkeycolumns and must not treat INCLUDE attrs as an ordered prefix.
-- Use a separate table so src's index set stays unchanged.
--
CREATE TABLE inc (
    k int4,
    ts timestamp
);

INSERT INTO inc
SELECT i, '2020-01-01'::timestamp + (i || ' hours')::interval
FROM generate_series(1, 100) i;

CREATE INDEX inc_k_inc_ts_idx ON inc (k) INCLUDE (ts);
CREATE INDEX inc_ts_idx ON inc (ts);
ANALYZE inc;

explain (costs off, verbose)
select k, ts from inc order by k;

explain (costs off, verbose)
select k, ts::date from inc order by k, ts::date;

explain (costs off, verbose)
select ts::date from inc order by ts::date;

--
-- Test DISTINCT with monotonic functions
--

explain (costs off, verbose)
select distinct ts::date from src;

--
-- Test window functions with PARTITION BY monotonic function
--

explain (costs off, verbose)
select ts::date, ts, row_number() over (partition by ts::date order by ts)
from src;

--
-- RelabelType across binary-compatible types with different default
-- orders: int4 (signed) vs oid (unsigned).  Index order on int4 is not
-- oid order, so SLOPE must not omit the Sort.
--
EXPLAIN (COSTS OFF)
SELECT v_int4::oid FROM src ORDER BY 1;



--
-- Now that some plans were shown, and we see that in many cases what
-- we care is whether a plan has a index scan or not. This function will
-- check that for us and just return
--
CREATE OR REPLACE FUNCTION index_plan (query text) RETURNS BOOLEAN LANGUAGE plpgsql AS $$
DECLARE
    plan json;
BEGIN
   EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) SELECT ' || query || ' ORDER BY 1' into plan;
   RETURN  plan->0->'Plan'->>'Node Type' LIKE 'Index%Scan';
END
$$;


--
-- Functions that take a timezone argument
--
PREPARE query(text) AS
WITH
cols AS (
    SELECT * FROM unnest(ARRAY['ts', 'tstz'])
        WITH ORDINALITY AS c(col, ord)
),
units AS (
    SELECT * FROM unnest(ARRAY['year', 'month', 'day', 'hour', 'minute', 'second'])
        WITH ORDINALITY AS s(unit, ord)
)
SELECT
    f AS expression,
    coalesce(string_agg(col, ', ' ORDER BY cols.ord) FILTER (
        WHERE index_plan(format('timezone(%L, %I) FROM src', $1, col))
    ), 'none') AS monotonic
FROM cols,
     unnest(ARRAY['timezone(TZ, <>)']) AS f(f)
GROUP BY f
UNION ALL
SELECT
    'date_trunc(<>, tstz, TZ)' AS expression,
    coalesce(string_agg(unit, ', ' ORDER BY units.ord) FILTER (
        WHERE index_plan(format('date_trunc(%L, tstz, %L) FROM src', unit, $1))
    ), 'none') AS monotonic
FROM units
ORDER BY 1;

EXECUTE query('UTC');
EXECUTE query('Africa/Ouagadougou');
EXECUTE query('Europe/London');
EXECUTE query('Antarctica/Troll');
EXECUTE query('Pacific/Guam');
EXECUTE query('America/Goose_Bay');

--
-- Functions that depend on session timezone
--
PREPARE query_local AS
WITH
cols AS (
    SELECT * FROM unnest(ARRAY['ts', 'tstz'])
        WITH ORDINALITY AS c(col, ord)
),
units AS (
    SELECT * FROM unnest(ARRAY['year', 'month', 'day', 'hour', 'minute', 'second'])
        WITH ORDINALITY AS s(unit, ord)
)
SELECT
    format(f, '<>') AS expression,
    coalesce(string_agg(col, ', ' ORDER BY cols.ord) FILTER (
        WHERE index_plan(format(f, col) || ' FROM src')
    ), 'none') AS monotonic
FROM cols,
     unnest(ARRAY['%s AT LOCAL', 'timezone(%s)', '%s::date']) AS f(f)
GROUP BY f
UNION ALL
SELECT
    format('date_trunc(<>, %s)', col) AS expression,
    coalesce(string_agg(unit, ', ' ORDER BY units.ord) FILTER (
        WHERE index_plan(format('date_trunc(%L, %I) FROM src', unit, col))
    ), 'none') AS monotonic
FROM cols, units
GROUP BY col
ORDER BY 1;

-- UTC
SET timezone = 'UTC';
EXECUTE query_local;

-- No DST at all
SET timezone = 'Africa/Ouagadougou';
EXECUTE query_local;

-- The common case: one hour DST
SET timezone = 'Europe/London';
EXECUTE query_local;

-- DST might affect the hour
SET timezone = 'Antarctica/Troll';
EXECUTE query_local;

-- A weird case: used to go back a day
SET timezone = 'Pacific/Guam';
EXECUTE query_local;

-- Even weirder, went back a month
SET timezone = 'America/Goose_Bay';
EXECUTE query_local;

--
-- TimeZone lookup error handling
--
-- Errors should not be thrown at planning time
-- if the timezone lookup fails.

EXPLAIN (COSTS OFF)
SELECT min(date_trunc('day', ts, 'Bogus/Zone')) FROM src;
SELECT min(date_trunc('day', ts, 'Bogus/Zone')) FROM src;

DEALLOCATE ALL;
DROP SCHEMA slope CASCADE;