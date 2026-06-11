--
-- WAIT_EVENT_TIMING
--
-- Exercises the wait_event_capture instrumentation: the GUC, the stats
-- surface (pg_stat_get_wait_event_timing(), the pg_stat_wait_event_timing
-- and histogram-buckets views, overflow counters, resets), and the trace
-- surface (the per-session ring, its readers, and the query markers).
--
-- Two expected outputs are maintained:
--   wait_event_timing.out    -- --enable-wait-event-timing builds
--   wait_event_timing_1.out  -- builds without the option (stub)
-- The difference is whether SET wait_event_capture = stats succeeds and
-- whether the SRF records anything; durations are never printed, so the
-- timing-build output is deterministic.
--

-- Default is off.
SHOW wait_event_capture;

-- The taxonomy view is pure SQL and identical in both build configs.
SELECT count(*) AS buckets FROM pg_wait_event_timing_histogram_buckets;
SELECT bucket_idx, lower_ns, upper_ns, label
FROM pg_wait_event_timing_histogram_buckets
WHERE bucket_idx IN (0, 1, 31)
ORDER BY bucket_idx;

-- Enable stats capture and generate a deterministic wait: pg_sleep emits a
-- Timeout / PgSleep wait of ~0.1s.  (In a stub build the SET errors and the
-- SRF stays empty; that is the documented difference between the two
-- expected files.)
SET wait_event_capture = stats;
SELECT pg_sleep(0.1);

-- PgSleep must now be recorded for this backend, with the per-event
-- invariants holding.  We print only booleans so the output is stable.
SELECT calls >= 1 AS calls_ok,
       calls = (SELECT sum(h) FROM unnest(histogram) AS h) AS hist_sum_eq_calls,
       total_time_ms > 0 AS total_positive,
       max_time_us > 0 AS max_positive,
       array_length(histogram, 1)
         = (SELECT count(*)::int FROM pg_wait_event_timing_histogram_buckets)
         AS histogram_len_ok
FROM pg_stat_get_wait_event_timing(pg_backend_pid())
WHERE wait_event = 'PgSleep';

-- The view surfaces the same row (type/name only; durations omitted).
SELECT wait_event_type, wait_event
FROM pg_stat_wait_event_timing
WHERE pid = pg_backend_pid() AND wait_event = 'PgSleep';

-- A non-NULL pid that does not exist yields no rows (silent, not an error).
SELECT count(*) AS rows_for_bogus_pid
FROM pg_stat_get_wait_event_timing(-1);

-- Overflow/reset counters for this backend.  A simple test backend uses
-- few LWLock tranches and no out-of-range classes, so both overflow
-- counters are zero, and a fresh backend has not been reset.
SELECT lwlock_overflow_count, flat_overflow_count, reset_count
FROM pg_stat_wait_event_timing_overflow
WHERE pid = pg_backend_pid();

-- Resetting our own backend is synchronous: the PgSleep row is cleared and
-- reset_count advances.  (We filter to PgSleep because inter-command waits
-- such as ClientRead may be recorded again before the next statement runs.)
SELECT pg_stat_reset_wait_event_timing(NULL);
SELECT count(*) AS pgsleep_rows_after_reset
FROM pg_stat_wait_event_timing
WHERE pid = pg_backend_pid() AND wait_event = 'PgSleep';
SELECT reset_count
FROM pg_stat_wait_event_timing_overflow
WHERE pid = pg_backend_pid();

-- The pid argument defaults to NULL, so a no-argument call resets the
-- caller's own backend.
SELECT pg_stat_reset_wait_event_timing();

-- Resetting an unknown pid is a silent no-op, not an error.
SELECT pg_stat_reset_wait_event_timing(2147483647);

RESET wait_event_capture;

--
-- Trace level: per-session ring of individual waits + query markers.
-- (In a stub build SET trace errors and the trace readers stay empty;
-- that is the documented difference between the two expected files.)
--
SET compute_query_id = on;
SET wait_event_capture = trace;
SELECT pg_sleep(0.1);

-- PgSleep is recorded in this backend's ring with a positive duration.
SELECT count(*) >= 1 AS pgsleep_in_ring,
       coalesce(bool_and(duration_us > 0), false) AS durations_positive
FROM pg_get_backend_wait_event_trace()
WHERE wait_event = 'PgSleep';

-- The same records are visible through the view.
SELECT count(*) >= 1 AS view_has_pgsleep
FROM pg_backend_wait_event_trace
WHERE wait_event = 'PgSleep';

-- Executor markers bracket the work and carry a non-zero query_id.
SELECT count(*) >= 1 AS has_exec_markers,
       coalesce(bool_and(query_id <> 0), true) AS markers_have_query_id
FROM pg_get_backend_wait_event_trace()
WHERE wait_event_type = 'Query'
  AND wait_event IN ('ExecStart', 'ExecEnd');

-- Clearing orphaned rings is a no-op here (no orphans) but must succeed.
SELECT pg_stat_clear_orphaned_wait_event_rings() >= 0 AS clear_orphans_ok;

RESET compute_query_id;
RESET wait_event_capture;
