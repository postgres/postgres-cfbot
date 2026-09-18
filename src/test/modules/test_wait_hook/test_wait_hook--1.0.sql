/* src/test/modules/test_wait_hook/test_wait_hook--1.0.sql */

\echo Use "CREATE EXTENSION test_wait_hook" to load this file. \quit

--
-- test_wait_hook_install(consumer)
--
-- Installs a test consumer of the wait_event_begin_hook/wait_event_end_hook
-- pair.  consumer must be 'A' or 'B'.  Installing 'B' while 'A' (or any
-- other hook) is already installed chains 'B' onto it, per the contract
-- documented in src/include/utils/wait_event.h.
--
CREATE FUNCTION test_wait_hook_install(consumer text)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_hook_install'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

--
-- test_wait_hook_uninstall_all()
--
-- Undoes every test_wait_hook_install() call in this session, in reverse
-- order, restoring whatever hooks were previously in place.
--
CREATE FUNCTION test_wait_hook_uninstall_all()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_hook_uninstall_all'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

--
-- test_wait_hook_events()
--
-- Returns the events recorded by the installed consumers, in the order
-- they occurred, and empties the ring.
--
CREATE FUNCTION test_wait_hook_events(
    OUT kind text,
    OUT consumer text,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT depth int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'test_wait_hook_events'
LANGUAGE C VOLATILE PARALLEL UNSAFE ROWS 64;

--
-- test_wait_hook_nested_wait_in_hook(enable)
--
-- Toggles a deliberate contract violation in consumer A's begin hook: while
-- enabled, the hook itself waits on the process latch, to demonstrate that
-- the depth guard in wait_event.h prevents the resulting nested wait from
-- re-entering the hooks.
--
CREATE FUNCTION test_wait_hook_nested_wait_in_hook(enable boolean)
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_hook_nested_wait_in_hook'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

--
-- test_wait_hook_error_after_start()
--
-- Reports the start of a wait and then raises an error without reporting
-- the matching end, to exercise the cleanup path that AbortTransaction()
-- performs on the caller's behalf.
--
CREATE FUNCTION test_wait_hook_error_after_start()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_hook_error_after_start'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

--
-- test_wait_hook_wait()
--
-- Waits on the process latch for exactly one WaitLatch() call (a fixed
-- 1ms timeout, no WL_LATCH_SET), reporting WAIT_EVENT_PG_SLEEP.  Unlike
-- pg_sleep(), an already-set process latch cannot make this return early
-- or loop around for a second wait, so it always produces exactly one
-- begin/end pair.
--
CREATE FUNCTION test_wait_hook_wait()
RETURNS void
AS 'MODULE_PATHNAME', 'test_wait_hook_wait'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

REVOKE ALL ON FUNCTION test_wait_hook_install(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_hook_uninstall_all() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_hook_events() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_hook_nested_wait_in_hook(boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_hook_error_after_start() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_wait_hook_wait() FROM PUBLIC;
