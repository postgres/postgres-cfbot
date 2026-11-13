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
-- The totals count from the last pg_stat_reset_wait_event_totals() (a scan
-- based cut: see the documentation) or from server start.  The snapshot,
-- diff and report functions give interval reports with per-wait and
-- per-transaction ratios; transactions are captured transaction completions
-- (commit and abort; prepare is counted apart).
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
    OUT capturing_backends integer,
    OUT reset_generation bigint,
    OUT stats_reset timestamptz,
    OUT reset_scan_start timestamptz,
    OUT xact_commit bigint,
    OUT xact_abort bigint,
    OUT xact_prepare bigint)
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
        t.capturing_backends,
        t.reset_generation,
        t.stats_reset,
        t.reset_scan_start,
        t.xact_commit,
        t.xact_abort,
        t.xact_prepare
    FROM pg_stat_get_wait_event_totals_info() t;
REVOKE ALL ON pg_stat_wait_event_totals_info FROM PUBLIC;
GRANT SELECT ON pg_stat_wait_event_totals_info TO pg_read_all_stats;

-- Superuser only, like pg_stat_reset_wait_event_timing_all().  Does not touch
-- the per-backend statistics, and the per-backend resets never touch these.
CREATE FUNCTION pg_stat_reset_wait_event_totals()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_reset_wait_event_totals'
LANGUAGE C VOLATILE;
REVOKE EXECUTE ON FUNCTION pg_stat_reset_wait_event_totals() FROM PUBLIC;

-- The totals as one jsonb document with exact integers.  scan_start and
-- scan_end are wall-clock microseconds since 2000-01-01 00:00:00 UTC, taken
-- before and after the scan.
CREATE FUNCTION pg_wait_event_snapshot()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_wait_event_snapshot'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
REVOKE EXECUTE ON FUNCTION pg_wait_event_snapshot() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_snapshot() TO pg_read_all_stats;

-- Validate two snapshots for pg_wait_event_diff() and return the interval
-- length and the transactions between them.  Interval length is the
-- difference of the scan midpoints (wall clock), in seconds.
CREATE FUNCTION pg_wait_event_diff_check(a jsonb, b jsonb,
    OUT interval_s double precision, OUT xacts bigint)
LANGUAGE plpgsql
PARALLEL RESTRICTED
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    s jsonb;
    lbl text;
    i int;
    bad int;
    dups int;
    scalar_keys text[] := ARRAY['schema_version', 'reset_generation',
        'scan_start', 'scan_end', 'xact_commit', 'xact_abort', 'xact_prepare'];
BEGIN
    FOR i IN 1..2 LOOP
        s := CASE i WHEN 1 THEN a ELSE b END;
        lbl := CASE i WHEN 1 THEN 'first' ELSE 'second' END;

        IF s IS NULL OR jsonb_typeof(s) <> 'object'
           OR NOT (s ?& (scalar_keys || ARRAY['incarnation', 'lwlock_overflow',
                                              'flat_overflow', 'events']))
           OR jsonb_typeof(s->'incarnation') <> 'string'
           OR jsonb_typeof(s->'events') <> 'array'
           OR jsonb_typeof(s->'lwlock_overflow') <> 'object'
           OR jsonb_typeof(s->'flat_overflow') <> 'object'
           OR NOT ((s->'lwlock_overflow') ?& ARRAY['calls', 'total_ns'])
           OR NOT ((s->'flat_overflow') ?& ARRAY['calls', 'total_ns']) THEN
            RAISE EXCEPTION 'the % argument is not a wait event snapshot', lbl;
        END IF;

        IF s->'schema_version' <> '1'::jsonb THEN
            RAISE EXCEPTION 'the % snapshot has unsupported schema_version %',
                lbl, s->'schema_version';
        END IF;

        IF EXISTS (SELECT 1 FROM jsonb_array_elements(s->'events') el
                   WHERE jsonb_typeof(el) <> 'object'
                      OR NOT (el ?& ARRAY['event_key', 'type', 'name',
                                          'calls', 'total_ns'])
                      OR jsonb_typeof(el->'type') <> 'string'
                      OR jsonb_typeof(el->'name') <> 'string') THEN
            RAISE EXCEPTION 'the % snapshot has a malformed event', lbl;
        END IF;

        -- Every counter must be an integer in 0 .. 2^63 - 1.
        SELECT count(*) INTO bad FROM (
            SELECT value AS v FROM jsonb_each(s) WHERE key = ANY (scalar_keys)
            UNION ALL SELECT value FROM jsonb_each(s->'lwlock_overflow')
            UNION ALL SELECT value FROM jsonb_each(s->'flat_overflow')
            UNION ALL SELECT e.value FROM jsonb_array_elements(s->'events') el,
                 jsonb_each(el) e WHERE e.key IN ('event_key', 'calls', 'total_ns')
        ) q
        WHERE jsonb_typeof(v) <> 'number'
           OR (v #>> '{}')::numeric <> trunc((v #>> '{}')::numeric)
           OR (v #>> '{}')::numeric NOT BETWEEN 0 AND 9223372036854775807;
        IF bad > 0 THEN
            RAISE EXCEPTION 'the % snapshot has % value(s) that are not integers in the bigint range',
                lbl, bad;
        END IF;

        -- Event keys are full wait event codes: integers in 0 .. 2^32 - 1.
        IF EXISTS (SELECT 1 FROM jsonb_array_elements(s->'events') el
                   WHERE (el->>'event_key')::numeric NOT BETWEEN 0 AND 4294967295) THEN
            RAISE EXCEPTION 'the % snapshot has an event key outside the 32-bit range', lbl;
        END IF;

        SELECT count(*) INTO dups FROM (
            SELECT el->'event_key' FROM jsonb_array_elements(s->'events') el
            GROUP BY 1 HAVING count(*) > 1) d;
        IF dups > 0 THEN
            RAISE EXCEPTION 'the % snapshot has duplicate event keys', lbl;
        END IF;
    END LOOP;

    IF a->>'incarnation' <> b->>'incarnation' THEN
        RAISE EXCEPTION 'the snapshots are from different server incarnations (% and %)',
            a->>'incarnation', b->>'incarnation';
    END IF;
    IF a->'reset_generation' <> b->'reset_generation' THEN
        RAISE EXCEPTION 'the totals were reset between the snapshots (reset_generation % and %)',
            a->'reset_generation', b->'reset_generation';
    END IF;
    IF (a->>'scan_end')::numeric < (a->>'scan_start')::numeric
       OR (b->>'scan_end')::numeric < (b->>'scan_start')::numeric THEN
        RAISE EXCEPTION 'a snapshot ends before it starts';
    END IF;
    IF (b->>'scan_start')::numeric < (a->>'scan_end')::numeric THEN
        RAISE EXCEPTION 'the snapshots overlap or are in the wrong order (the second scan started before the first one ended)';
    END IF;

    xacts := ((b->>'xact_commit')::numeric - (a->>'xact_commit')::numeric)
           + ((b->>'xact_abort')::numeric - (a->>'xact_abort')::numeric);
    IF (b->>'xact_commit')::numeric < (a->>'xact_commit')::numeric
       OR (b->>'xact_abort')::numeric < (a->>'xact_abort')::numeric
       OR (b->>'xact_prepare')::numeric < (a->>'xact_prepare')::numeric THEN
        RAISE EXCEPTION 'a transaction counter decreased between the snapshots';
    END IF;
    interval_s := (((b->>'scan_start')::numeric + (b->>'scan_end')::numeric)
                 - ((a->>'scan_start')::numeric + (a->>'scan_end')::numeric))
                  / 2 / 1000000;
END;
$$;
REVOKE EXECUTE ON FUNCTION pg_wait_event_diff_check(jsonb, jsonb) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_diff_check(jsonb, jsonb) TO pg_read_all_stats;

-- What happened between two snapshots, per event.
--   waits           completed waits in the interval
--   wait_time_ms    their total time
--   ms_per_wait     wait_time_ms / waits
--   waits_per_xact  waits / xacts
--   ms_per_xact     wait_time_ms / xacts
--   xacts           captured transaction completions, commit + abort
--                   (prepare is not counted), over all processes
-- Ratios are NULL when their denominator is zero.  Rows with no change are
-- omitted.  The two overflow rows (keys -1 and -2) are included.  Unless
-- include_idle, the wait event type Activity and the Client event
-- ClientRead are left out; the transaction denominator is never filtered.
-- An event missing from the first snapshot counts from zero; one missing
-- from the second, or any counter that decreased, is an error.
CREATE FUNCTION pg_wait_event_diff(a jsonb, b jsonb, include_idle boolean DEFAULT false)
RETURNS TABLE (
    event_key bigint,
    wait_event_type text,
    wait_event text,
    waits bigint,
    wait_time_ms double precision,
    ms_per_wait double precision,
    waits_per_xact double precision,
    ms_per_xact double precision,
    interval_s double precision,
    xacts bigint)
LANGUAGE plpgsql
PARALLEL RESTRICTED
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    chk record;
    r record;
BEGIN
    SELECT * INTO chk FROM @extschema@.pg_wait_event_diff_check(a, b);

    FOR r IN
        WITH ea AS (
            SELECT (el->>'event_key')::bigint AS k, el->>'type' AS t, el->>'name' AS n,
                   (el->>'calls')::numeric AS c, (el->>'total_ns')::numeric AS ns
            FROM jsonb_array_elements(a->'events') el
            UNION ALL SELECT -1, 'LWLock', '(overflow)',
                   (a->'lwlock_overflow'->>'calls')::numeric,
                   (a->'lwlock_overflow'->>'total_ns')::numeric
            UNION ALL SELECT -2, '(overflow)', '(overflow)',
                   (a->'flat_overflow'->>'calls')::numeric,
                   (a->'flat_overflow'->>'total_ns')::numeric
        ), eb AS (
            SELECT (el->>'event_key')::bigint AS k, el->>'type' AS t, el->>'name' AS n,
                   (el->>'calls')::numeric AS c, (el->>'total_ns')::numeric AS ns
            FROM jsonb_array_elements(b->'events') el
            UNION ALL SELECT -1, 'LWLock', '(overflow)',
                   (b->'lwlock_overflow'->>'calls')::numeric,
                   (b->'lwlock_overflow'->>'total_ns')::numeric
            UNION ALL SELECT -2, '(overflow)', '(overflow)',
                   (b->'flat_overflow'->>'calls')::numeric,
                   (b->'flat_overflow'->>'total_ns')::numeric
        )
        SELECT eb.k, eb.t, eb.n, eb.c - coalesce(ea.c, 0) AS dc,
               eb.ns - coalesce(ea.ns, 0) AS dns, NULL::bigint AS missing
        FROM eb LEFT JOIN ea USING (k)
        UNION ALL
        SELECT ea.k, ea.t, ea.n, 0, 0, ea.k
        FROM ea WHERE ea.k NOT IN (SELECT k FROM eb)
        ORDER BY 1
    LOOP
        IF r.missing IS NOT NULL THEN
            RAISE EXCEPTION 'event % of the first snapshot is missing from the second', r.missing;
        END IF;
        IF r.dc < 0 OR r.dns < 0 THEN
            RAISE EXCEPTION 'the counters of event % decreased between the snapshots', r.k;
        END IF;
        CONTINUE WHEN r.dc = 0 AND r.dns = 0;
        CONTINUE WHEN include_idle IS NOT TRUE
                      AND (r.t = 'Activity' OR (r.t = 'Client' AND r.n = 'ClientRead'));

        event_key := r.k;
        wait_event_type := r.t;
        wait_event := r.n;
        waits := r.dc;
        wait_time_ms := (r.dns / 1000000)::double precision;
        ms_per_wait := CASE WHEN r.dc > 0
                            THEN (r.dns / 1000000 / r.dc)::double precision END;
        waits_per_xact := CASE WHEN chk.xacts > 0
                               THEN (r.dc / chk.xacts)::double precision END;
        ms_per_xact := CASE WHEN chk.xacts > 0
                            THEN (r.dns / 1000000 / chk.xacts)::double precision END;
        interval_s := chk.interval_s;
        xacts := chk.xacts;
        RETURN NEXT;
    END LOOP;
END;
$$;
REVOKE EXECUTE ON FUNCTION pg_wait_event_diff(jsonb, jsonb, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_diff(jsonb, jsonb, boolean) TO pg_read_all_stats;

-- Snapshot, sleep, snapshot, diff.  (The plpgsql functions here use a fixed
-- search_path and reach the extension's own functions through @extschema@,
-- which is why the extension is not relocatable.)  Raises a NOTICE with the interval, the
-- transactions and the transactions per second; that is the only place a
-- result without rows shows them.  Run it from a session with capture off,
-- or its own sleep is counted.
CREATE FUNCTION pg_wait_event_report(seconds integer DEFAULT 10, include_idle boolean DEFAULT false)
RETURNS TABLE (
    event_key bigint,
    wait_event_type text,
    wait_event text,
    waits bigint,
    wait_time_ms double precision,
    ms_per_wait double precision,
    waits_per_xact double precision,
    ms_per_xact double precision,
    interval_s double precision,
    xacts bigint)
LANGUAGE plpgsql
STRICT
PARALLEL RESTRICTED
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    s1 jsonb;
    s2 jsonb;
    chk record;
BEGIN
    IF seconds < 1 THEN
        RAISE EXCEPTION 'seconds must be at least 1';
    END IF;
    s1 := @extschema@.pg_wait_event_snapshot();
    PERFORM pg_catalog.pg_sleep(seconds);
    s2 := @extschema@.pg_wait_event_snapshot();
    SELECT * INTO chk FROM @extschema@.pg_wait_event_diff_check(s1, s2);
    RAISE NOTICE 'interval % s, % transactions, % transactions per second',
        round(chk.interval_s::numeric, 3), chk.xacts,
        CASE WHEN chk.interval_s > 0
             THEN round((chk.xacts / chk.interval_s)::numeric, 2) END;
    RETURN QUERY SELECT * FROM @extschema@.pg_wait_event_diff(s1, s2, include_idle);
END;
$$;
REVOKE EXECUTE ON FUNCTION pg_wait_event_report(integer, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_wait_event_report(integer, boolean) TO pg_read_all_stats;
