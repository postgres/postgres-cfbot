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

DROP ROLE regress_pwet_plain;
DROP ROLE regress_pwet_stats;
