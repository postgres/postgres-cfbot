--
-- Smoke test of the producer functions without any wait-event consumer.
--
CREATE EXTENSION test_wait_totals;

-- Some CI configurations force debug_parallel_query = regress, which would
-- run the PARALLEL SAFE emitters below in a worker; this test needs them here.
SET debug_parallel_query = off;

SELECT test_wait_totals_event_key() > 0 AS has_key;

SELECT test_wait_totals_emit(3);
SELECT test_wait_totals_emit(0);
SELECT test_wait_totals_emit(2, 50);
SELECT test_wait_totals_emit(-1);
-- Limits are checked before any pair opens.
SELECT test_wait_totals_emit(1000000001);
SELECT test_wait_totals_emit(1, 10000001);

-- Outside a parallel worker, the row emitter reports 0.
SELECT test_wait_totals_emit_row(2, 1) AS in_worker;

SELECT test_wait_totals_emit_then_cpu(2, 5);

-- The worker emits and exits; the call returns after it has exited.
SELECT test_wait_totals_bgworker(4);

SELECT test_wait_totals_procnumber() >= 0 AS has_procnumber;

-- Tranche helpers.
SELECT array_length(test_wait_totals_new_tranches(3, 'smoke'), 1) AS tranches;
SELECT test_wait_totals_emit_lwlock(test_wait_totals_new_tranches(2, 'smoke2'), 2);
SELECT test_wait_totals_lock_holder(0);
SELECT test_wait_totals_lock_hold(0, 1, 10);
SELECT test_wait_totals_lock_wait(0);
SELECT test_wait_totals_lock_hold(9);

-- Handshakes: an emitter whose gate is already open does not hold.
SELECT test_wait_totals_gate_reset();
SELECT test_wait_totals_holding();
SELECT test_wait_totals_release();
SELECT test_wait_totals_emit_hold(2);
SELECT test_wait_totals_holding();
SELECT test_wait_totals_gate_reset();
SELECT test_wait_totals_stop();
SELECT test_wait_totals_emit_until_stopped() > 0 AS ran;
SELECT test_wait_totals_wait_writers(1);
SELECT test_wait_totals_emit_event(test_wait_totals_event_key(), 2);
SELECT test_wait_totals_wait_progress(0) > 0 AS progressed;
SELECT test_wait_totals_bgworker_procnumber() >= 0 AS has_worker_procnumber;
