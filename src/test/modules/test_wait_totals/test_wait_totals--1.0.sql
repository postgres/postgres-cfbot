/* src/test/modules/test_wait_totals/test_wait_totals--1.0.sql */

\echo Use "CREATE EXTENSION test_wait_totals" to load this file. \quit

--
-- The wait_event_info code of the dedicated extension wait event
-- "TestWaitTotals", which every emitter below uses.
--
CREATE FUNCTION test_wait_totals_event_key()
RETURNS bigint
AS 'MODULE_PATHNAME', 'test_wait_totals_event_key'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

--
-- test_wait_totals_emit(n, spin_us)
--
-- Exactly n timed start/end pairs of the dedicated event, busy-spinning
-- spin_us microseconds inside each, so their total time is positive.
-- PARALLEL SAFE: it keeps no state besides the process-local event id.
--
CREATE FUNCTION test_wait_totals_emit(n int, spin_us int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

--
-- test_wait_totals_emit_row(n, spin_us)
--
-- As above, but returns 1 when it ran in a parallel worker and 0 otherwise.
-- Meant to be called once per row of a forced parallel scan: the sum of the
-- result is the number of rows that really ran in a worker.
--
CREATE FUNCTION test_wait_totals_emit_row(n int DEFAULT 1, spin_us int DEFAULT 1)
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_row'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

--
-- test_wait_totals_emit_then_cpu(n, cpu_ms, spin_us)
--
-- n pairs, then cpu_ms milliseconds of CPU in the same call with no wait
-- (interruptible), so the last completed wait is still unpublished.
--
CREATE FUNCTION test_wait_totals_emit_then_cpu(n int, cpu_ms int, spin_us int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_then_cpu'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

--
-- test_wait_totals_bgworker(n)
--
-- Starts a background worker that emits n pairs and exits right after the
-- last one, and returns once the worker has exited.
--
CREATE FUNCTION test_wait_totals_bgworker(n int)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_bgworker'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

--
-- Like test_wait_totals_bgworker(), but the worker stays alive (no wait)
-- after emitting until test_wait_totals_release(); returns once it started.
--
CREATE FUNCTION test_wait_totals_bgworker_hold(n int)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_bgworker_hold'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_procnumber()
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_procnumber'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

--
-- LWLock tranche helpers.  test_wait_totals_new_tranches() registers n new
-- tranches and returns their ids; test_wait_totals_emit_lwlock() reports
-- completed waits on given tranches without needing contention.  For real
-- contention, one session runs test_wait_totals_lock_hold() while another
-- calls test_wait_totals_lock_wait() on the same lock index.
--
CREATE FUNCTION test_wait_totals_new_tranches(n int, prefix text DEFAULT 'twt')
RETURNS int[]
AS 'MODULE_PATHNAME', 'test_wait_totals_new_tranches'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_emit_lwlock(tranche_ids int[], reps int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_lwlock'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_lock_tranche(idx int)
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_lock_tranche'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_lock_hold(idx int, waiters int DEFAULT 1, timeout_ms int DEFAULT 20000)
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_lock_hold'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_lock_holder(idx int)
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_lock_holder'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_lock_wait(idx int)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_lock_wait'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

--
-- Test-only handshakes.  test_wait_totals_emit_hold() emits and then stays
-- in a CPU loop (no wait) until test_wait_totals_release(); holding() tells
-- an observer the emitter is inside that loop.  Writers started with
-- emit_until_stopped() run until test_wait_totals_stop() and return their
-- count; wait_writers(n) waits until n of them run.  All waits are bounded
-- by a failure watchdog only.  gate_reset() clears every flag.
--
CREATE FUNCTION test_wait_totals_gate_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_gate_reset'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_holding()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_wait_totals_holding'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_release()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_release'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_emit_hold(n int, spin_us int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_hold'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_emit_until_stopped(spin_us int DEFAULT 1)
RETURNS bigint
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_until_stopped'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_wait_writers(n int)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_wait_writers'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_stop()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_stop'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

--
-- Needs pg_wait_event_tracing.capture.  emit_with_capture() sets the level,
-- emits synthetic LWLock waits and restores the level without returning to
-- SQL in between; emit_then_set_capture() emits and changes the level so the
-- last completion is still pending at the change; emit_event() emits an
-- arbitrary wait event code.
--
CREATE FUNCTION test_wait_totals_emit_with_capture(mode text, tranche_ids int[], reps int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_with_capture'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_emit_then_set_capture(n int, mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_then_set_capture'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_emit_event(event_key bigint, n int DEFAULT 1)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_totals_emit_event'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_wait_progress(prev bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'test_wait_totals_wait_progress'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION test_wait_totals_bgworker_procnumber()
RETURNS int
AS 'MODULE_PATHNAME', 'test_wait_totals_bgworker_procnumber'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

REVOKE ALL ON FUNCTION test_wait_totals_event_key() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit(int, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_row(int, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_then_cpu(int, int, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_bgworker(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_procnumber() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_new_tranches(int, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_lwlock(int[], int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_lock_tranche(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_lock_hold(int, int, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_lock_holder(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_lock_wait(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_gate_reset() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_holding() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_release() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_hold(int, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_until_stopped(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_wait_writers(int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_stop() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_with_capture(text, int[], int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_then_set_capture(int, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_emit_event(bigint, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_wait_progress(bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_bgworker_procnumber() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_totals_bgworker_hold(int) FROM PUBLIC;
