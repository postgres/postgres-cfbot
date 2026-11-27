--
-- PG_WAIT_EVENT_TRACING_TOTALS
--
-- Extension version 1.1: the instance-wide totals views.  Installation and
-- upgrade, the shape of the views, the reserved overflow keys, the
-- incarnation format and the privileges.  Counting itself is exercised by
-- the TAP tests of src/test/modules/test_wait_totals, where a dedicated wait
-- event makes exact assertions possible; here only things that hold whatever
-- other tests did to this server are asserted.
--

-- CI configurations may force debug_parallel_query = regress; keep this
-- session's statements in this session.
SET debug_parallel_query = off;

-- Version handling: 1.0 has no totals objects, the update adds them, and a
-- fresh install of the default version has the same objects.
DROP EXTENSION IF EXISTS pg_wait_event_tracing;
CREATE EXTENSION pg_wait_event_tracing VERSION '1.0';
SELECT extversion FROM pg_extension WHERE extname = 'pg_wait_event_tracing';
SELECT count(*) AS totals_objects_in_1_0
FROM pg_proc WHERE proname LIKE 'pg_stat_get_wait_event_totals%';

ALTER EXTENSION pg_wait_event_tracing UPDATE TO '1.1';
SELECT extversion FROM pg_extension WHERE extname = 'pg_wait_event_tracing';
SELECT count(*) AS totals_objects_after_update
FROM pg_proc WHERE proname LIKE 'pg_stat_get_wait_event_totals%';

DROP EXTENSION pg_wait_event_tracing;
CREATE EXTENSION pg_wait_event_tracing;
SELECT extversion FROM pg_extension WHERE extname = 'pg_wait_event_tracing';
SELECT count(*) AS totals_objects_fresh
FROM pg_proc WHERE proname LIKE 'pg_stat_get_wait_event_totals%';

-- Columns of the two views.
SELECT attname, format_type(atttypid, atttypmod) AS type
FROM pg_attribute
WHERE attrelid = 'pg_stat_wait_event_totals'::regclass AND attnum > 0
ORDER BY attnum;

SELECT attname, format_type(atttypid, atttypmod) AS type
FROM pg_attribute
WHERE attrelid = 'pg_stat_wait_event_totals_info'::regclass AND attnum > 0
ORDER BY attnum;

-- One info row; the incarnation is 16 lower-case hex digits.
SELECT count(*) AS info_rows FROM pg_stat_wait_event_totals_info;
SELECT incarnation ~ '^[0-9a-f]{16}$' AS incarnation_format,
       invalid_ledgers,
       ledger_slots > 0 AS has_slots,
       capturing_backends >= 0 AS has_capturing
FROM pg_stat_wait_event_totals_info;

-- The incarnation does not change while the server runs.
SELECT incarnation AS incarnation_before FROM pg_stat_wait_event_totals_info \gset
SELECT incarnation = :'incarnation_before' AS incarnation_stable
FROM pg_stat_wait_event_totals_info;

-- The two overflow rows exist with their reserved keys, and no other row has
-- a negative key; every other key is a full wait event code, which carries
-- the wait event class in its top byte.
SELECT event_key, wait_event_type, wait_event, calls >= 0 AS calls_ok
FROM pg_stat_wait_event_totals
WHERE event_key < 0
ORDER BY event_key;

SELECT count(*) AS bad_keys
FROM pg_stat_wait_event_totals
WHERE event_key >= 0 AND (event_key >= 4294967296 OR event_key < 16777216);

SELECT count(*) AS duplicate_keys
FROM (SELECT event_key FROM pg_stat_wait_event_totals
      GROUP BY event_key HAVING count(*) > 1) d;

-- Counters are never negative, and the average is NULL exactly when there
-- are no calls.
SELECT count(*) AS bad_rows
FROM pg_stat_wait_event_totals
WHERE calls < 0 OR total_time_ms < 0 OR (calls = 0) <> (avg_time_us IS NULL);

-- Privileges: not readable by PUBLIC, neither the views nor the functions
-- behind them; readable with pg_read_all_stats.
CREATE ROLE regress_pwet_plain;
CREATE ROLE regress_pwet_stats IN ROLE pg_read_all_stats;

SELECT has_function_privilege('regress_pwet_plain',
                              'pg_stat_get_wait_event_totals()', 'EXECUTE') AS plain_totals_fn,
       has_function_privilege('regress_pwet_plain',
                              'pg_stat_get_wait_event_totals_info()', 'EXECUTE') AS plain_info_fn,
       has_function_privilege('regress_pwet_stats',
                              'pg_stat_get_wait_event_totals()', 'EXECUTE') AS stats_totals_fn,
       has_function_privilege('regress_pwet_stats',
                              'pg_stat_get_wait_event_totals_info()', 'EXECUTE') AS stats_info_fn;

SET ROLE regress_pwet_plain;
SELECT count(*) FROM pg_stat_wait_event_totals;
SELECT count(*) FROM pg_stat_wait_event_totals_info;
SELECT count(*) FROM pg_stat_get_wait_event_totals();
SELECT count(*) FROM pg_stat_get_wait_event_totals_info();
RESET ROLE;

SET ROLE regress_pwet_stats;
SELECT count(*) > 0 AS totals_visible FROM pg_stat_wait_event_totals;
SELECT count(*) AS info_visible FROM pg_stat_wait_event_totals_info;
RESET ROLE;

-- Phase 2 functions: PUBLIC denied, pg_read_all_stats may read, only a
-- superuser may reset.
SELECT p.proname,
       has_function_privilege('regress_pwet_plain', p.oid, 'EXECUTE') AS plain,
       has_function_privilege('regress_pwet_stats', p.oid, 'EXECUTE') AS stats
FROM pg_proc p
WHERE p.proname IN ('pg_wait_event_snapshot', 'pg_wait_event_diff',
                    'pg_wait_event_diff_check', 'pg_wait_event_report',
                    'pg_stat_reset_wait_event_totals')
ORDER BY p.proname;

SET ROLE regress_pwet_plain;
SELECT pg_wait_event_snapshot();
SELECT pg_stat_reset_wait_event_totals();
RESET ROLE;
SET ROLE regress_pwet_stats;
SELECT jsonb_typeof(pg_wait_event_snapshot()) AS snapshot_type;
SELECT pg_stat_reset_wait_event_totals();
RESET ROLE;

DROP ROLE regress_pwet_plain;
DROP ROLE regress_pwet_stats;

--
-- Snapshot, diff and report (phase 2).
--
SELECT array_agg(k ORDER BY k) AS snapshot_keys
FROM jsonb_object_keys(pg_wait_event_snapshot()) k;
SELECT pg_wait_event_snapshot()->'schema_version' AS schema_version,
       jsonb_typeof(pg_wait_event_snapshot()->'events') AS events_type,
       (pg_wait_event_snapshot()->>'incarnation') = (SELECT incarnation FROM pg_stat_wait_event_totals_info) AS same_incarnation;

-- Reset: the generation advances by one and the scan times appear.
SELECT reset_generation AS gen_before FROM pg_stat_wait_event_totals_info \gset
SELECT pg_stat_reset_wait_event_totals();
SELECT reset_generation = :gen_before + 1 AS generation_advanced,
       stats_reset IS NOT NULL AS has_stats_reset,
       reset_scan_start <= stats_reset AS scan_times_ordered,
       xact_commit >= 0 AND xact_abort >= 0 AND xact_prepare >= 0 AS xact_columns
FROM pg_stat_wait_event_totals_info;

-- Crafted snapshots.  a: 10 commits, 2 aborts; b: 15 and 3, i.e. 6 transactions.
SELECT '{"schema_version":1,"incarnation":"00000000000000aa","reset_generation":0,
 "scan_start":100,"scan_end":200,"xact_commit":10,"xact_abort":2,"xact_prepare":1,
 "lwlock_overflow":{"calls":0,"total_ns":0},"flat_overflow":{"calls":0,"total_ns":0},
 "events":[{"event_key":117440513,"type":"Extension","name":"X","calls":4,"total_ns":4000000},
           {"event_key":83886080,"type":"Activity","name":"A","calls":5,"total_ns":1000000},
           {"event_key":100663296,"type":"Client","name":"ClientRead","calls":1,"total_ns":1000000},
           {"event_key":50331648,"type":"Lock","name":"relation","calls":7,"total_ns":7000000}]}'::jsonb AS a \gset
SELECT jsonb_set(jsonb_set(jsonb_set(jsonb_set(jsonb_set(jsonb_set(:'a'::jsonb,
  '{scan_start}', '300'), '{scan_end}', '400'),
  '{xact_commit}', '15'), '{xact_abort}', '3'),
  '{lwlock_overflow}', '{"calls":3,"total_ns":3000000}'),
  '{events}', '[{"event_key":117440513,"type":"Extension","name":"X","calls":10,"total_ns":10000000},
                {"event_key":117440514,"type":"Extension","name":"Y","calls":3,"total_ns":3000000},
                {"event_key":83886080,"type":"Activity","name":"A","calls":9,"total_ns":2000000},
                {"event_key":100663296,"type":"Client","name":"ClientRead","calls":4,"total_ns":4000000},
                {"event_key":50331648,"type":"Lock","name":"relation","calls":7,"total_ns":7000000}]') AS b \gset

-- waits_per_xact = waits / xacts, ms_per_xact = wait_time_ms / xacts, with
-- xacts = commit delta + abort delta = 6; interval_s from the scan midpoints.
SELECT * FROM pg_wait_event_diff(:'a', :'b');
SELECT * FROM pg_wait_event_diff(:'a', :'b', true);
-- A NULL include_idle is not true: idle rows are left out, like false.
SELECT event_key FROM pg_wait_event_diff(:'a', :'b', NULL);

-- Zero transactions: ratios per transaction are NULL.
SELECT event_key, ms_per_wait, waits_per_xact, ms_per_xact, xacts
FROM pg_wait_event_diff(:'a', jsonb_set(jsonb_set(:'b'::jsonb, '{xact_commit}', '10'), '{xact_abort}', '2'));

-- Zero events and no change: no rows.
SELECT count(*) AS no_rows FROM pg_wait_event_diff(
  jsonb_set(:'a'::jsonb, '{events}', '[]'),
  jsonb_set(jsonb_set(:'b'::jsonb, '{events}', '[]'), '{lwlock_overflow}', '{"calls":0,"total_ns":0}'));

-- Errors.
\set VERBOSITY terse
SELECT * FROM pg_wait_event_diff(:'b', :'a');
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{scan_start}', '150'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{incarnation}', '"00000000000000bb"'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{reset_generation}', '1'));
SELECT * FROM pg_wait_event_diff(jsonb_set(:'a'::jsonb, '{schema_version}', '2'), :'b');
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,calls}', '1.5'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,calls}', '-1'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,calls}', '9223372036854775808'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,calls}', '"7"'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,event_key}', '4294967296'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,0,event_key}', '-1'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events}',
  (:'b'::jsonb->'events') || (:'b'::jsonb->'events'->0)));
SELECT * FROM pg_wait_event_diff(:'a', :'b'::jsonb #- '{events,0,name}');
SELECT * FROM pg_wait_event_diff(:'a', :'b'::jsonb - 'xact_abort');
SELECT * FROM pg_wait_event_diff('[]', :'b');
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events}', '[]'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{events,4,calls}', '6'));
SELECT * FROM pg_wait_event_diff(:'a', jsonb_set(:'b'::jsonb, '{xact_commit}', '9'));

-- Report: argument validation (the sleep itself is exercised by the TAP tests).
SELECT * FROM pg_wait_event_report(0);
SELECT * FROM pg_wait_event_report(-5, true);
SELECT count(*) AS null_arg_rows FROM pg_wait_event_report(NULL);
SELECT count(*) AS null_idle_rows FROM pg_wait_event_report(1, NULL);
\set VERBOSITY default

-- The extension is not relocatable: its report functions call each other by
-- the extension schema, so they work from a schema that is not in the
-- search_path.
RESET ALL;
DROP EXTENSION pg_wait_event_tracing;
CREATE SCHEMA pwet_s;
CREATE EXTENSION pg_wait_event_tracing SCHEMA pwet_s;
SELECT extrelocatable FROM pg_extension WHERE extname = 'pg_wait_event_tracing';
SET search_path = pg_catalog;
-- the report's NOTICE carries the measured interval, which is never stable;
-- what a report returns is checked by the TAP tests
SET client_min_messages = warning;
SELECT count(*) IS NOT NULL AS report_runs_from_other_schema FROM pwet_s.pg_wait_event_report(1);
-- the rows of the crafted snapshots from above, through the other schema
SELECT event_key, waits, xacts FROM pwet_s.pg_wait_event_diff(:'a', :'b') ORDER BY event_key;
RESET client_min_messages;
RESET search_path;
ALTER EXTENSION pg_wait_event_tracing SET SCHEMA public;
DROP EXTENSION pg_wait_event_tracing;
DROP SCHEMA pwet_s;
CREATE EXTENSION pg_wait_event_tracing;
