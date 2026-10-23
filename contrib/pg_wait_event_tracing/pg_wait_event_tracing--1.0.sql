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

-- Query-attribution view over a procnumber's trace ring (fix 6).  Each
-- statement's waits are summed per wait event into a bucket named by the
-- statement's opening marker.  The rule, applied to the markers in seq
-- order:
--
--  * Utility level L counts open top-level-or-nested utility statements:
--    UtilityStart adds one (after the marker), UtilityEnd subtracts one
--    but never below zero, Idle and TxnAbort reset it.  TxnCommit does
--    not (a COMMIT inside a procedure leaves its CALL open).  L at a
--    marker is the value before that marker is applied.  A marker is
--    "top-level" when its depth is 0 and L is 0: statements run by a
--    DO block, a CALL, an EXECUTE or CREATE TABLE AS then belong to the
--    outer statement, whose UtilityStart does not raise the executor
--    depth but does raise L.
--
--  * A top-level QueryStart always opens a statement bucket.  A top-level
--    UtilityStart or ExecStart opens one unless the nearest preceding
--    marker among the top-level QueryStart/UtilityStart/ExecStart, Idle,
--    TxnCommit and TxnAbort is a top-level QueryStart or UtilityStart (that
--    statement is already open).  So an EXECUTE of a prepared statement
--    over the extended protocol (ExecStart only, no QueryStart) opens a
--    bucket, and so does an ExecStart right after TxnCommit with no Idle
--    in between (the next message was already buffered), but a simple
--    query's QueryStart/ExecStart pair, or a plan revalidation, is one
--    bucket.
--
--  * Idle and TxnAbort open the synthetic '<idle>' bucket.  Waits before
--    the first of these or of an opener are '<unattributed>'.
--
--  * Pre-statement phase.  Between the arrival of a request and its
--    opening marker the backend can already wait: plan cache locks in
--    Bind, relation locks in parse analysis.  Take an opener O and the
--    nearest closer C (Idle, TxnCommit or TxnAbort) before it; if no other
--    opener lies between them, the waits between C and O whose type is not
--    'Client' belong to O, with phase 'pre'.  Client waits (ClientRead and
--    the like) are idle time and stay in '<idle>'.
--
--  * A request that fails before any statement starts (lock_timeout
--    during Bind, say) has the waits between its Idle and the following
--    TxnAbort or TxnCommit, with no opener in between and type other than
--    'Client', in the synthetic '<no-statement>' bucket.
--
--  * Every other wait stays in the bucket of the latest opener, Idle or
--    TxnAbort, so the waits after ExecEnd or TxnCommit but before Idle
--    (commit WAL flush) stay with their statement.
--
-- Output column phase: 'pre' for the moved waits, 'statement' for the
-- rest of a statement bucket, NULL for the synthetic buckets.
-- Every non-'Query' record in the ring is counted exactly once.
--
-- Known ambiguities: with no commit between two statements of an
-- explicit transaction or pipeline, waits between the end of one
-- statement and the start marker of the next stay with the earlier one;
-- when the next request was already buffered (no Idle), end-of-
-- transaction work after TxnCommit counts as the next statement's 'pre'
-- phase; an error caught by a PL/pgSQL EXCEPTION block inside a utility
-- statement can leave L too high until the next Idle or TxnAbort, which
-- folds later statements of that top-level statement's call into it; and
-- two Parse messages pipelined ahead of their Executes (QueryStart,
-- QueryStart, ExecStart, ExecStart) attribute the first Execute to the
-- second Parse.
--
-- Grants match the underlying pg_get_wait_event_trace(): PUBLIC revoked,
-- pg_read_all_stats granted (this is a read-only view over the same
-- data, just pre-aggregated).
CREATE FUNCTION pg_wait_event_trace_by_statement(
    procnumber int4,
    OUT bucket text,
    OUT statement_seq int8,
    OUT query_id int8,
    OUT phase text,
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
    SELECT seq, wait_event_type, wait_event, duration_us, query_id, depth,
           (wait_event_type = 'Query') AS is_marker
    FROM pg_get_wait_event_trace(procnumber)
),
-- Utility level L: a running sum clamped at zero, restarted at every
-- Idle/TxnAbort.  For a walk clamped at zero, L = P - min(0, min P) where P
-- is the plain running sum.
lvl1 AS (
    SELECT t.*,
           count(*) FILTER (WHERE is_marker AND wait_event IN ('Idle', 'TxnAbort'))
               OVER (ORDER BY seq ROWS UNBOUNDED PRECEDING) AS ugrp,
           CASE WHEN is_marker AND wait_event = 'UtilityStart' THEN 1
                WHEN is_marker AND wait_event = 'UtilityEnd' THEN -1
                ELSE 0 END AS dl
    FROM trace t
),
lvl2 AS (
    SELECT l.*,
           sum(dl) OVER (PARTITION BY ugrp ORDER BY seq ROWS UNBOUNDED PRECEDING) AS p
    FROM lvl1 l
),
lvl3 AS (
    SELECT l.*,
           p - least(0, min(p) OVER (PARTITION BY ugrp ORDER BY seq
                                     ROWS UNBOUNDED PRECEDING)) AS l_after
    FROM lvl2 l
),
lvl4 AS (
    SELECT l.*,
           coalesce(lag(l_after) OVER (PARTITION BY ugrp ORDER BY seq), 0) AS l_before
    FROM lvl3 l
),
kinds AS (
    SELECT seq, wait_event_type, wait_event, duration_us, query_id, is_marker,
           (is_marker AND wait_event IN ('Idle', 'TxnCommit', 'TxnAbort')) AS is_closer,
           CASE WHEN is_marker AND depth = 0 AND l_before = 0
                THEN CASE wait_event WHEN 'QueryStart' THEN 'QS'
                                     WHEN 'UtilityStart' THEN 'US'
                                     WHEN 'ExecStart' THEN 'ES' END
           END AS start_kind,
           CASE WHEN is_marker AND wait_event IN ('Idle', 'TxnCommit', 'TxnAbort')
                THEN wait_event END AS closer_kind
    FROM lvl4
),
-- The marker immediately before each reference marker (top-level start or
-- closer), among reference markers only.
refs AS (
    SELECT seq,
           lag(coalesce(start_kind, closer_kind)) OVER (ORDER BY seq) AS prev_ref
    FROM kinds
    WHERE start_kind IS NOT NULL OR closer_kind IS NOT NULL
),
opened AS (
    SELECT k.*,
           (k.start_kind = 'QS'
            OR (k.start_kind IN ('US', 'ES')
                AND coalesce(r.prev_ref, '') NOT IN ('QS', 'US'))) AS is_opener,
           (k.is_marker AND k.wait_event IN ('Idle', 'TxnAbort')) AS is_idle_open
    FROM kinds k
    LEFT JOIN refs r USING (seq)
),
-- bucket_group numbers the v11-style buckets (latest opener/Idle/TxnAbort);
-- cl_in/cl_ex number the closer-delimited segments, counting the closer
-- itself as the end (cl_ex) or the start (cl_in) of its segment.
grouped AS (
    SELECT o.*,
           count(*) FILTER (WHERE is_opener OR is_idle_open)
               OVER (ORDER BY seq ROWS UNBOUNDED PRECEDING) AS bucket_group,
           count(*) FILTER (WHERE is_closer)
               OVER (ORDER BY seq ROWS UNBOUNDED PRECEDING) AS cl_in
    FROM opened o
),
seg AS (
    SELECT g.*,
           cl_in - (CASE WHEN is_closer THEN 1 ELSE 0 END) AS cl_ex
    FROM grouped g
),
seg2 AS (
    SELECT s.*,
           min(bucket_group) FILTER (WHERE is_opener) OVER (PARTITION BY cl_ex)
               AS seg_opener_group,
           min(seq) FILTER (WHERE is_opener) OVER (PARTITION BY cl_ex)
               AS seg_opener_seq,
           max(closer_kind) OVER (PARTITION BY cl_in) AS seg_start_kind,
           max(closer_kind) OVER (PARTITION BY cl_ex) AS seg_end_kind
    FROM seg s
),
assigned AS (
    SELECT s.*,
           CASE
               WHEN NOT is_marker AND wait_event_type <> 'Client'
                    AND cl_ex >= 1 AND seg_opener_seq > seq
                   THEN seg_opener_group
               WHEN NOT is_marker AND wait_event_type <> 'Client'
                    AND seg_start_kind = 'Idle' AND seg_opener_seq IS NULL
                    AND seg_end_kind IN ('TxnAbort', 'TxnCommit')
                   THEN -1
               ELSE bucket_group
           END AS eff_group,
           coalesce(NOT is_marker AND wait_event_type <> 'Client'
                    AND cl_ex >= 1 AND seg_opener_seq > seq, false) AS is_pre
    FROM seg2 s
),
bucket_labels AS (
    SELECT bucket_group,
           seq AS statement_seq,
           query_id AS bucket_query_id,
           is_idle_open
    FROM assigned
    WHERE is_opener OR is_idle_open
)
SELECT
    CASE
        WHEN a.eff_group = -1 THEN '<no-statement>'
        WHEN a.eff_group = 0 THEN '<unattributed>'
        WHEN bl.is_idle_open THEN '<idle>'
        ELSE bl.statement_seq::text
    END AS bucket,
    CASE WHEN NOT bl.is_idle_open THEN bl.statement_seq END AS statement_seq,
    CASE WHEN NOT bl.is_idle_open THEN bl.bucket_query_id END AS query_id,
    CASE WHEN a.eff_group > 0 AND NOT bl.is_idle_open
         THEN CASE WHEN a.is_pre THEN 'pre' ELSE 'statement' END
    END AS phase,
    a.wait_event_type,
    a.wait_event,
    count(*) AS calls,
    sum(a.duration_us) AS total_time_us
FROM assigned a
LEFT JOIN bucket_labels bl ON bl.bucket_group = a.eff_group
WHERE NOT a.is_marker
GROUP BY a.eff_group, bl.statement_seq, bl.bucket_query_id, bl.is_idle_open,
         (a.is_pre), a.wait_event_type, a.wait_event
ORDER BY min(a.seq), a.wait_event_type, a.wait_event;
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
