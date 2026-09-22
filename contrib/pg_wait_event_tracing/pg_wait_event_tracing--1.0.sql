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

-- The histogram bucket boundaries are a constant lookup table, and the
-- per-class capacities are compile-time limits of this module; neither
-- says anything about any session, so both stay readable by everyone,
-- as the equivalent view was before this module existed.
GRANT SELECT ON pg_wait_event_timing_histogram_buckets TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_tracing_capacity() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_tracing_hooks_installed() TO PUBLIC;
