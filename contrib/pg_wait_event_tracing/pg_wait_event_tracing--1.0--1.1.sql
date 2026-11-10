/* contrib/pg_wait_event_tracing/pg_wait_event_tracing--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_wait_event_tracing UPDATE TO '1.1'" to load this file. \quit

-- Instance-wide cumulative wait totals.  Unlike pg_stat_wait_event_timing,
-- which has one row set per live collecting backend and forgets a backend
-- when it exits, resets or stops collecting, these totals sum a lifetime
-- ledger kept per process slot, so they include processes that have already
-- exited.  They start from zero at every server start (including a crash
-- restart); the incarnation in the info view changes when they do.
--
-- event_key is the full 32-bit wait event code (the class in the top byte).
-- Two reserved negative keys stand for events that did not fit: -1 is the
-- LWLock overflow row (tranches beyond pg_wait_event_tracing.max_tranches
-- in a process) and -2 the flat overflow row (events outside the flat table);
-- both keep calls and time.  Both rows are always present.
--
-- NOTE: version 1.1 is not final.  The remaining objects of this version
-- (transaction counters, reset, snapshot, diff and report) are added to this
-- same script before it is released.
CREATE FUNCTION pg_stat_get_wait_event_totals(
    OUT wait_event_type text,
    OUT wait_event text,
    OUT event_key bigint,
    OUT calls bigint,
    OUT total_time_ms double precision,
    OUT avg_time_us double precision)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_get_wait_event_totals'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
REVOKE EXECUTE ON FUNCTION pg_stat_get_wait_event_totals() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_stat_get_wait_event_totals() TO pg_read_all_stats;

CREATE VIEW pg_stat_wait_event_totals AS
    SELECT
        t.wait_event_type,
        t.wait_event,
        t.event_key,
        t.calls,
        t.total_time_ms,
        t.avg_time_us
    FROM pg_stat_get_wait_event_totals() t;
REVOKE ALL ON pg_stat_wait_event_totals FROM PUBLIC;
GRANT SELECT ON pg_stat_wait_event_totals TO pg_read_all_stats;

CREATE FUNCTION pg_stat_get_wait_event_totals_info(
    OUT incarnation text,
    OUT lwlock_overflow_calls bigint,
    OUT lwlock_overflow_time_ms double precision,
    OUT flat_overflow_calls bigint,
    OUT flat_overflow_time_ms double precision,
    OUT invalid_ledgers bigint,
    OUT ledger_slots integer,
    OUT capturing_backends integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_get_wait_event_totals_info'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
REVOKE EXECUTE ON FUNCTION pg_stat_get_wait_event_totals_info() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_stat_get_wait_event_totals_info() TO pg_read_all_stats;

CREATE VIEW pg_stat_wait_event_totals_info AS
    SELECT
        t.incarnation,
        t.lwlock_overflow_calls,
        t.lwlock_overflow_time_ms,
        t.flat_overflow_calls,
        t.flat_overflow_time_ms,
        t.invalid_ledgers,
        t.ledger_slots,
        t.capturing_backends
    FROM pg_stat_get_wait_event_totals_info() t;
REVOKE ALL ON pg_stat_wait_event_totals_info FROM PUBLIC;
GRANT SELECT ON pg_stat_wait_event_totals_info TO pg_read_all_stats;
