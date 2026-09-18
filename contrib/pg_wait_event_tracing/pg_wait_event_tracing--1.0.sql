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
--  * TxnCommit and TxnAbort with a nonzero depth were written inside a
--    running utility statement (COMMIT or ROLLBACK in a procedure or DO
--    block, the internal transactions of VACUUM or CREATE INDEX
--    CONCURRENTLY).  That statement goes on, so these markers are ignored
--    by every rule below.  "TxnCommit" and "TxnAbort" mean depth 0.
--
--  * IdleInTransaction is Idle written while a transaction that can still
--    commit is open (an explicit transaction block, or the implicit
--    transaction of the extended protocol waiting for its Sync).  Every
--    rule below that names Idle applies to it too, except where it is
--    named separately.
--
--  * Utility level L counts open top-level-or-nested utility statements:
--    UtilityStart adds one (after the marker), UtilityEnd subtracts one
--    but never below zero, Idle and TxnAbort reset it.  TxnCommit does
--    not.  UtilityEnd is written on error too, so L is exact as long as
--    the ring holds the whole utility statement (see the ambiguities
--    below for when it does not).  L at a
--    marker is the value before that marker is applied.  A marker is
--    "top-level" when its depth is 0 and L is 0: statements run by a
--    DO block, a CALL, an EXECUTE or CREATE TABLE AS then belong to the
--    outer statement, whose UtilityStart does not raise the executor
--    depth but does raise L.
--
--  * A top-level QueryStart always opens a statement bucket.  A top-level
--    UtilityStart or ExecStart opens one unless the nearest preceding
--    marker among the top-level QueryStart/UtilityStart/ExecStart, the
--    UtilityEnd at depth 0 that brings L back to 0 (UtilityEnd carries the
--    executor depth around the utility statement: a SHOW that a function
--    runs while a plan is made does not end anything), Idle, TxnCommit and
--    TxnAbort is a top-level QueryStart or UtilityStart (that statement is
--    already open).  So an EXECUTE of a prepared statement
--    over the extended protocol (ExecStart only, no QueryStart) opens a
--    bucket, and so does an ExecStart right after TxnCommit with no Idle
--    in between (the next message was already buffered), but a simple
--    query's QueryStart/ExecStart pair, or a plan revalidation, is one
--    bucket; a prepared statement executed right after a CALL in the same
--    pipeline (UtilityEnd, then ExecStart) is a bucket of its own.
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
--  * Delayed commit.  An extended-protocol client can send its Sync only
--    after the backend has waited for input: the statement has ended, an
--    IdleInTransaction follows, and the commit that the Sync runs comes
--    after it.  When a statement opened between the closer before the
--    IdleInTransaction and the IdleInTransaction itself, and no opener
--    lies between the IdleInTransaction and the next TxnCommit, the waits
--    of that segment and of the segment from the TxnCommit to the next
--    closer (if it has no opener either) whose type is not 'Client' go to
--    the bucket of the latest opener, the transaction's last statement,
--    with phase 'statement': the same bucket they get when the Sync
--    arrives with the statement.  Client waits stay in '<idle>'.
--
--  * A request that fails before any statement starts (lock_timeout
--    during Bind, say) has the waits between its Idle and the following
--    TxnAbort or TxnCommit, with no opener in between and type other than
--    'Client', in the synthetic '<no-statement>' bucket.  After an
--    IdleInTransaction this applies to a following TxnAbort, and to a
--    TxnCommit that is not a delayed commit.
--
--  * Every other wait stays in the bucket of the latest opener, Idle or
--    TxnAbort, so the waits after ExecEnd (the commit WAL flush and
--    synchronous replication wait, which precede TxnCommit) and after
--    TxnCommit (cleanup after the commit) but before Idle stay with their
--    statement.
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
-- phase, and a request that fails before any statement starts (the
-- '<no-statement>' case) is not recognized, its waits staying with the
-- preceding statement, since only an Idle marker tells it from that
-- statement's end-of-transaction work; two Parse messages pipelined
-- ahead of their Executes (QueryStart, QueryStart, ExecStart, ExecStart)
-- attribute the first Execute to the second Parse; a commit that fails
-- at a delayed Sync (a deferred constraint, a serialization failure)
-- cannot be told from a request that fails, so its waits go to
-- '<no-statement>', while the same failure with the Sync sent at once
-- stays with the statement; a non-Client wait during the pause before a
-- delayed Sync (interrupt processing) goes with the commit to the last
-- statement; an extended-protocol SELECT keeps its executor open until
-- the commit or the next Bind, so no IdleInTransaction is written for it
-- and the pause before a delayed Sync (or between Executes with a row
-- limit) stays in its bucket, as Client waits; statements run by deferred
-- triggers at commit open buckets of their own; and when the ring holds
-- the inside of a utility statement but not its UtilityStart (capture was
-- switched to trace while it ran, or the ring wrapped past it), L is too
-- low until the next Idle or TxnAbort, so the statements it runs read as
-- top-level.
--
-- The rule is pg_wait_event_trace_attribute(), applied to an array of
-- trace records; pg_wait_event_trace_by_statement() applies it to a
-- backend's ring.  The first works on its argument alone, so it keeps the
-- default EXECUTE grant (it is also how the rule is tested on fixed marker
-- streams).  The second has the grants of the underlying
-- pg_get_wait_event_trace(): PUBLIC revoked, pg_read_all_stats granted
-- (it is a read-only view over the same data, just pre-aggregated).
CREATE TYPE pg_wait_event_trace_record AS (
    seq int8,
    wait_event_type text,
    wait_event text,
    duration_us float8,
    query_id int8,
    depth int4);

CREATE FUNCTION pg_wait_event_trace_attribute(
    records pg_wait_event_trace_record[],
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
IMMUTABLE
PARALLEL SAFE
AS $$
WITH trace AS (
    SELECT seq, wait_event_type,
           -- a transaction end inside a running utility statement is not a
           -- statement boundary: give it a name no rule below looks at
           CASE WHEN wait_event_type = 'Query' AND depth > 0
                     AND wait_event IN ('TxnCommit', 'TxnAbort')
                THEN 'TxnInUtility' ELSE wait_event END AS wait_event,
           duration_us, query_id, depth,
           (wait_event_type = 'Query') AS is_marker
    FROM unnest(records)
),
-- Utility level L: a running sum clamped at zero, restarted at every
-- Idle/IdleInTransaction/TxnAbort.  For a walk clamped at zero,
-- L = P - min(0, min P) where P is the plain running sum.
lvl1 AS (
    SELECT t.*,
           count(*) FILTER (WHERE is_marker AND wait_event IN
                               ('Idle', 'IdleInTransaction', 'TxnAbort'))
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
           (is_marker AND wait_event IN ('Idle', 'IdleInTransaction',
                                         'TxnCommit', 'TxnAbort')) AS is_closer,
           CASE WHEN is_marker AND depth = 0 AND l_before = 0
                THEN CASE wait_event WHEN 'QueryStart' THEN 'QS'
                                     WHEN 'UtilityStart' THEN 'US'
                                     WHEN 'ExecStart' THEN 'ES' END
           END AS start_kind,
           CASE WHEN is_marker AND wait_event IN ('Idle', 'IdleInTransaction',
                                                  'TxnCommit', 'TxnAbort')
                THEN wait_event END AS closer_kind,
           CASE WHEN is_marker AND wait_event = 'UtilityEnd' AND depth = 0
                     AND l_after = 0
                THEN 'UE' END AS end_kind
    FROM lvl4
),
-- The marker immediately before each reference marker (top-level start,
-- end of a top-level utility statement, or closer), among reference markers
-- only.
refs AS (
    SELECT seq,
           lag(coalesce(start_kind, end_kind, closer_kind)) OVER (ORDER BY seq)
               AS prev_ref
    FROM kinds
    WHERE start_kind IS NOT NULL OR end_kind IS NOT NULL
          OR closer_kind IS NOT NULL
),
opened AS (
    SELECT k.*,
           (k.start_kind = 'QS'
            OR (k.start_kind IN ('US', 'ES')
                AND coalesce(r.prev_ref, '') NOT IN ('QS', 'US'))) AS is_opener,
           (k.is_marker AND k.wait_event IN ('Idle', 'IdleInTransaction',
                                             'TxnAbort')) AS is_idle_open
    FROM kinds k
    LEFT JOIN refs r USING (seq)
),
-- bucket_group numbers the v11-style buckets (latest opener/Idle/
-- IdleInTransaction/TxnAbort);
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
-- owner_group is the bucket of the latest opener, for the delayed commit
seg AS (
    SELECT g.*,
           cl_in - (CASE WHEN is_closer THEN 1 ELSE 0 END) AS cl_ex,
           max(bucket_group) FILTER (WHERE is_opener)
               OVER (ORDER BY seq ROWS UNBOUNDED PRECEDING) AS owner_group
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
-- One row per closer.  A TxnCommit ends a delayed commit when the segment
-- it ends has no opener and starts with an IdleInTransaction that ends a
-- segment with an opener (the previous closer's own segment).
closers AS (
    SELECT cl_in, cl_ex,
           (wait_event = 'TxnCommit' AND seg_opener_seq IS NULL
            AND coalesce(lag(wait_event = 'IdleInTransaction'
                             AND seg_opener_seq IS NOT NULL)
                             OVER (ORDER BY seq), false)) AS ends_delayed_commit
    FROM seg2
    WHERE is_closer
),
-- For a wait: is its segment a delayed commit (the closer ending it says
-- so), or the opener-free segment right after one (the closer starting it
-- says so)?
seg3 AS (
    SELECT s.*,
           coalesce(ce.ends_delayed_commit, false) AS in_delayed_commit,
           coalesce(cs.ends_delayed_commit AND s.seg_opener_seq IS NULL, false)
               AS after_delayed_commit
    FROM seg2 s
    LEFT JOIN closers ce ON NOT s.is_marker AND ce.cl_ex = s.cl_in
    LEFT JOIN closers cs ON NOT s.is_marker AND cs.cl_in = s.cl_in
),
assigned AS (
    SELECT s.*,
           CASE
               WHEN NOT is_marker AND wait_event_type <> 'Client'
                    AND cl_ex >= 1 AND seg_opener_seq > seq
                   THEN seg_opener_group
               WHEN NOT is_marker AND wait_event_type <> 'Client'
                    AND (in_delayed_commit OR after_delayed_commit)
                   THEN coalesce(owner_group, 0)
               WHEN NOT is_marker AND wait_event_type <> 'Client'
                    AND seg_start_kind IN ('Idle', 'IdleInTransaction')
                    AND seg_opener_seq IS NULL
                    AND seg_end_kind IN ('TxnAbort', 'TxnCommit')
                   THEN -1
               ELSE bucket_group
           END AS eff_group,
           coalesce(NOT is_marker AND wait_event_type <> 'Client'
                    AND cl_ex >= 1 AND seg_opener_seq > seq, false) AS is_pre
    FROM seg3 s
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
SELECT *
FROM @extschema@.pg_wait_event_trace_attribute(ARRAY(
    SELECT ROW(t.seq, t.wait_event_type, t.wait_event, t.duration_us,
               t.query_id, t.depth)::@extschema@.pg_wait_event_trace_record
    FROM @extschema@.pg_get_wait_event_trace(procnumber) t
    ORDER BY t.seq));
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
