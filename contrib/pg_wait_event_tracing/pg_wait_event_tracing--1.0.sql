/* contrib/pg_wait_event_tracing/pg_wait_event_tracing--1.0.sql */

\echo Use "CREATE EXTENSION pg_wait_event_tracing" to load this file. \quit

CREATE FUNCTION pg_stat_get_wait_event_timing(
    IN pid int4 DEFAULT NULL,
    OUT pid integer,
    OUT backend_type text,
    OUT procnumber integer,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT calls bigint,
    OUT total_time_ms double precision,
    OUT avg_time_us double precision,
    OUT max_time_us double precision,
    OUT histogram bigint[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_get_wait_event_timing'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE VIEW pg_stat_wait_event_timing AS
    SELECT
        t.pid,
        t.backend_type,
        t.procnumber,
        t.wait_event_type,
        t.wait_event,
        t.calls,
        t.total_time_ms,
        t.avg_time_us,
        t.max_time_us,
        t.histogram
    FROM pg_stat_get_wait_event_timing(NULL) t;
REVOKE ALL ON pg_stat_wait_event_timing FROM PUBLIC;
GRANT SELECT ON pg_stat_wait_event_timing TO pg_read_all_stats;

CREATE FUNCTION pg_stat_get_wait_event_timing_overflow(
    IN pid int4 DEFAULT NULL,
    OUT pid integer,
    OUT backend_type text,
    OUT procnumber integer,
    OUT lwlock_overflow_count bigint,
    OUT flat_overflow_count bigint,
    OUT reset_count bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_get_wait_event_timing_overflow'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE VIEW pg_stat_wait_event_timing_overflow AS
    SELECT
        t.pid,
        t.backend_type,
        t.procnumber,
        t.lwlock_overflow_count,
        t.flat_overflow_count,
        t.reset_count
    FROM pg_stat_get_wait_event_timing_overflow(NULL) t;
REVOKE ALL ON pg_stat_wait_event_timing_overflow FROM PUBLIC;
GRANT SELECT ON pg_stat_wait_event_timing_overflow TO pg_read_all_stats;

-- Taxonomy for the histogram column on pg_stat_wait_event_timing.  The
-- histogram array has one entry per bucket, in ascending order.  This view
-- names them so callers do not have to memorise the layout; join against it
-- via unnest(histogram) WITH ORDINALITY.
--
-- WARNING: keep this list in lock-step with PWET_HISTOGRAM_BUCKETS and
-- pwet_timing_bucket() in pg_wait_event_tracing.c.  Bin edges are powers of
-- two in nanoseconds; labels are the approximate decimal-microsecond grid.
CREATE VIEW pg_wait_event_timing_histogram_buckets AS
    SELECT bucket_idx, lower_ns, upper_ns, label
    FROM (VALUES
        ( 0,             0::bigint,         1024::bigint,  '<1us'::text),
        ( 1,          1024::bigint,         2048::bigint,  '1-2us'),
        ( 2,          2048::bigint,         4096::bigint,  '2-4us'),
        ( 3,          4096::bigint,         8192::bigint,  '4-8us'),
        ( 4,          8192::bigint,        16384::bigint,  '8-16us'),
        ( 5,         16384::bigint,        32768::bigint,  '16-32us'),
        ( 6,         32768::bigint,        65536::bigint,  '32-64us'),
        ( 7,         65536::bigint,       131072::bigint,  '64-128us'),
        ( 8,        131072::bigint,       262144::bigint,  '128-256us'),
        ( 9,        262144::bigint,       524288::bigint,  '256-512us'),
        (10,        524288::bigint,      1048576::bigint,  '512us-1ms'),
        (11,       1048576::bigint,      2097152::bigint,  '1-2ms'),
        (12,       2097152::bigint,      4194304::bigint,  '2-4ms'),
        (13,       4194304::bigint,      8388608::bigint,  '4-8ms'),
        (14,       8388608::bigint,     16777216::bigint,  '8-16ms'),
        (15,      16777216::bigint,     33554432::bigint,  '16-32ms'),
        (16,      33554432::bigint,     67108864::bigint,  '32-64ms'),
        (17,      67108864::bigint,    134217728::bigint,  '64-128ms'),
        (18,     134217728::bigint,    268435456::bigint,  '128-256ms'),
        (19,     268435456::bigint,    536870912::bigint,  '256-512ms'),
        (20,     536870912::bigint,   1073741824::bigint,  '512ms-1s'),
        (21,    1073741824::bigint,   2147483648::bigint,  '1-2s'),
        (22,    2147483648::bigint,   4294967296::bigint,  '2-4s'),
        (23,    4294967296::bigint,   8589934592::bigint,  '4-8s'),
        (24,    8589934592::bigint,  17179869184::bigint,  '8-16s'),
        (25,   17179869184::bigint,  34359738368::bigint,  '16-32s'),
        (26,   34359738368::bigint,  68719476736::bigint,  '32-64s'),
        (27,   68719476736::bigint, 137438953472::bigint,  '64-128s'),
        (28,  137438953472::bigint, 274877906944::bigint,  '128-256s'),
        (29,  274877906944::bigint, 549755813888::bigint,  '256-512s'),
        (30,  549755813888::bigint, 1099511627776::bigint, '512s-1024s'),
        (31, 1099511627776::bigint, NULL::bigint,          '>=1024s')
    ) AS t(bucket_idx, lower_ns, upper_ns, label);

CREATE FUNCTION pg_stat_reset_wait_event_timing(pid int4 DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_reset_wait_event_timing'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_stat_reset_wait_event_timing_all()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_reset_wait_event_timing_all'
LANGUAGE C VOLATILE;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_wait_event_timing_all() FROM PUBLIC;

-- Per-class capacity of the dense timing table, for comparison against
-- "SELECT type, count(*) FROM pg_wait_events GROUP BY type" (see the
-- module's "capacity" regression test).
CREATE FUNCTION pg_wait_event_tracing_capacity(
    OUT type text,
    OUT capacity int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_wait_event_tracing_capacity'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

-- Diagnostic for lazy, per-process hook installation: has the calling
-- backend installed its own wait-event hooks (see
-- pwet_install_wait_hooks() in the C code)?  Reveals nothing about any
-- other backend or about what is being recorded, so it stays readable by
-- everyone, like pg_wait_event_tracing_capacity() above.
CREATE FUNCTION pg_wait_event_tracing_hooks_installed()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_wait_event_tracing_hooks_installed'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

-- Trace level (pg_wait_event_tracing.capture = trace): a per-session ring
-- buffer of individual completed waits plus query-attribution markers.
-- Reading a session's trace exposes its query_id and wait sequence, which
-- can leak across SECURITY DEFINER call chains, so the view AND both
-- underlying SRFs are locked to pg_read_all_stats, matching v6.
CREATE FUNCTION pg_get_backend_wait_event_trace(
    OUT seq int8,
    OUT timestamp_ns int8,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT duration_us float8,
    OUT query_id int8,
    OUT depth int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_backend_wait_event_trace'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE VIEW pg_backend_wait_event_trace AS
    SELECT
        t.seq,
        t.timestamp_ns,
        t.wait_event_type,
        t.wait_event,
        t.duration_us,
        t.query_id,
        t.depth
    FROM pg_get_backend_wait_event_trace() t;
REVOKE ALL ON pg_backend_wait_event_trace FROM PUBLIC;
GRANT SELECT ON pg_backend_wait_event_trace TO pg_read_all_stats;
-- Revoke the session-local SRF itself, not just the view, so a role that
-- can enable trace cannot read its own ring via the function and bypass
-- the view.
REVOKE EXECUTE ON FUNCTION pg_get_backend_wait_event_trace() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_backend_wait_event_trace() TO pg_read_all_stats;

-- Cross-backend reader, keyed by procnumber (reads ACTIVE and ORPHANED
-- rings alike -- fix 3 -- tagging every row with owner_pid, the ring's
-- producer, live or, for an orphan, its last-known pid; see
-- pg_stat_clear_orphaned_wait_event_rings() below for the orphan
-- lifecycle).
CREATE FUNCTION pg_get_wait_event_trace(
    procnumber int4,
    OUT owner_pid int4,
    OUT seq int8,
    OUT timestamp_ns int8,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT duration_us float8,
    OUT query_id int8,
    OUT depth int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wait_event_trace'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
REVOKE EXECUTE ON FUNCTION pg_get_wait_event_trace(int4) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wait_event_trace(int4) TO pg_read_all_stats;

-- Administrative sweep (fix 3): free every trace ring whose owner has
-- exited.  Cluster-scope and mutating, so -- like
-- pg_stat_reset_wait_event_timing_all() -- it is superuser-only in C and
-- NOT granted to pg_read_all_stats.
CREATE FUNCTION pg_stat_clear_orphaned_wait_event_rings()
RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_clear_orphaned_wait_event_rings'
LANGUAGE C VOLATILE;
REVOKE EXECUTE ON FUNCTION pg_stat_clear_orphaned_wait_event_rings() FROM PUBLIC;

-- Query-attribution view over a procnumber's trace ring (fix 6), per plan
-- sec 5.3's rule: a statement's interval runs from its QueryStart (or
-- UtilityStart) marker to the earliest of the next Idle, the next
-- QueryStart/UtilityStart at depth 0, or TxnAbort; waits inside are
-- summed per wait event.  TxnAbort is treated the same as Idle for
-- bucketing (both open the synthetic '<idle>' bucket): the plan says
-- TxnAbort ends the current statement's interval but does not name a
-- bucket for whatever follows before the next real activity, and
-- treating it as "now idle" avoids inventing an undocumented third
-- bucket for what is, from an attribution standpoint, the same kind of
-- gap. Waits before the ring's first marker of any kind are
-- '<unattributed>'. Depth-0 gating on QueryStart/UtilityStart matters
-- because post_parse_analyze (and, much more rarely, ProcessUtility) can
-- itself fire from inside an already-open outer statement (SPI calls
-- from a SQL/PL function); only a top-level start closes the
-- previous top-level statement's interval.
--
-- Grants match the underlying pg_get_wait_event_trace(): PUBLIC revoked,
-- pg_read_all_stats granted (this is a read-only view over the same
-- data, just pre-aggregated).
CREATE FUNCTION pg_wait_event_trace_by_statement(
    procnumber int4,
    OUT bucket text,
    OUT statement_seq int8,
    OUT query_id int8,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT calls int8,
    OUT total_time_us float8)
RETURNS SETOF record
LANGUAGE SQL
VOLATILE
PARALLEL RESTRICTED
AS $$
WITH trace AS (
    SELECT * FROM pg_get_wait_event_trace(procnumber)
),
marked AS (
    SELECT
        seq,
        wait_event_type,
        wait_event,
        duration_us,
        query_id,
        (wait_event_type = 'Query'
         AND wait_event IN ('QueryStart', 'UtilityStart')
         AND depth = 0) AS is_stmt_open,
        (wait_event_type = 'Query'
         AND wait_event IN ('Idle', 'TxnAbort')) AS is_idle_open
    FROM trace
),
bucketed AS (
    SELECT
        m.*,
        count(*) FILTER (WHERE is_stmt_open OR is_idle_open)
            OVER (ORDER BY seq ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
            AS bucket_group
    FROM marked m
),
bucket_labels AS (
    SELECT
        bucket_group,
        CASE WHEN is_stmt_open THEN seq END AS statement_seq,
        CASE WHEN is_stmt_open THEN query_id END AS bucket_query_id,
        is_idle_open
    FROM bucketed
    WHERE is_stmt_open OR is_idle_open
)
SELECT
    CASE
        WHEN b.bucket_group = 0 THEN '<unattributed>'
        WHEN bl.is_idle_open THEN '<idle>'
        ELSE bl.statement_seq::text
    END AS bucket,
    bl.statement_seq,
    bl.bucket_query_id AS query_id,
    b.wait_event_type,
    b.wait_event,
    count(*) AS calls,
    sum(b.duration_us) AS total_time_us
FROM bucketed b
LEFT JOIN bucket_labels bl USING (bucket_group)
WHERE b.wait_event_type <> 'Query'
GROUP BY b.bucket_group, bl.statement_seq, bl.bucket_query_id, bl.is_idle_open,
         b.wait_event_type, b.wait_event
ORDER BY min(b.seq), b.wait_event_type, b.wait_event;
$$;
REVOKE EXECUTE ON FUNCTION pg_wait_event_trace_by_statement(int4) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_trace_by_statement(int4) TO pg_read_all_stats;

-- The histogram bucket boundaries are a constant lookup table, and the
-- per-class capacities are compile-time limits of this module; neither
-- says anything about any session, so both stay readable by everyone,
-- as the equivalent view was before this module existed.
GRANT SELECT ON pg_wait_event_timing_histogram_buckets TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_tracing_capacity() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_tracing_hooks_installed() TO PUBLIC;
