--
-- Test module for the wait_event_begin_hook/wait_event_end_hook contract
-- (see src/include/utils/wait_event.h).
--
-- test_wait_hook_wait() is used throughout as a single, deterministic
-- wait: it performs exactly one WaitLatch() call with no WL_LATCH_SET, so
-- it always produces exactly one begin/end pair per installed consumer,
-- with no timing-dependent counts.  (pg_sleep() is not used for this: its
-- loop passes WL_LATCH_SET, so an already-set process latch can make it
-- return early and loop around for a second wait.)  The ring only ever
-- records PgSleep events (see the comment on ring_push() in
-- test_wait_hook.c), which also keeps this session's own housekeeping
-- waits -- such as the ClientRead wait between statements -- out of the
-- results.
--
CREATE EXTENSION test_wait_hook;

--
-- 1. With no consumer installed, waiting produces no events.
--
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();

--
-- 2. One consumer installed: a single wait yields exactly one matched
-- begin/end pair, reporting the expected wait event.
--
SELECT test_wait_hook_install('A');
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();

--
-- 3. A second consumer chained on top of the first: begin hooks fire
-- outer-to-inner (A, then B) and end hooks fire inner-to-outer (B, then
-- A), per the chaining contract.
--
SELECT test_wait_hook_install('B');
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();

SELECT test_wait_hook_uninstall_all();

--
-- 4. A hook that violates the contract by waiting on the latch itself.
-- The depth guard stops that nested wait from re-entering the hooks, so
-- no extra begin/end appears; but the nested wait's own bookkeeping still
-- clears the raw wait-event state before the outer wait's end call reads
-- it back, so that outer end is filtered out too (see the comment in
-- consumerA_begin()).  Only the outer begin survives.
--
SELECT test_wait_hook_install('A');
SELECT test_wait_hook_nested_wait_in_hook(true);
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();
SELECT test_wait_hook_nested_wait_in_hook(false);

--
-- 5. An error raised right after a wait start, without a matching end,
-- still leaves the backend consistent: AbortTransaction() performs the
-- missing end call on the caller's behalf, and a later wait behaves
-- normally again.
--
SELECT test_wait_hook_error_after_start();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();

--
-- 6. After uninstalling, waiting produces no events again.
--
SELECT test_wait_hook_uninstall_all();
SELECT test_wait_hook_wait();
SELECT kind, consumer, wait_event, depth FROM test_wait_hook_events();

DROP EXTENSION test_wait_hook;
