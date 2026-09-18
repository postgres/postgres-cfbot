--
-- PG_WAIT_EVENT_TRACING_TRACE
--
-- Exercises the trace level's query-attribution markers (fix 6): the
-- QueryStart/ExecStart/ExecEnd/UtilityStart/UtilityEnd/TxnCommit/TxnAbort
-- marker set and pg_wait_event_trace_by_statement().  The ring buffer's own
-- seqlock/wrap/orphan-lifecycle machinery is exercised by TAP tests with
-- injection points (WP4), not here.
--
-- Idle is deliberately excluded from every comparison below (see the WHERE
-- clause in the pattern below): whether it appears at all depends on
-- whether the backend actually blocks in ClientRead, which depends on
-- whether the next message psql sends is already buffered by the time the
-- backend looks -- protocol structure (separate messages vs. one
-- multi-statement message) makes it likely but not deterministic either
-- way on a loaded CI runner. A TAP test, where the client can pause
-- deliberately between statements to force the wait, covers Idle instead
-- (WP4b).
--
-- Reading a session's own ring via SQL is necessarily self-referential:
-- every observing SELECT below writes its own QueryStart+ExecStart into
-- the ring (post_parse_analyze/ExecutorStart fire before its body runs),
-- and the "markN" SELECT used to record a starting ring position finishes
-- writing its own ExecEnd+TxnCommit (and possibly an Idle, excluded here
-- for the same reason as above) *after* that position was captured
-- (captured mid-execution, from inside its own target list).  Every case
-- below therefore uses the identical, fully deterministic pattern:
--
--   SELECT coalesce(max(seq), -1) AS markN FROM pg_backend_wait_event_trace \gset
--   <the statement(s) under test>
--   SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
--   FROM pg_backend_wait_event_trace
--   WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :markN;
--
-- [3:count(*)-2]: index 1-2 are always the "markN" statement's own
-- trailing ExecEnd, TxnCommit (everything it itself writes to the ring
-- after the position was captured from its still-in-progress ExecStart,
-- minus any Idle, already filtered out by the WHERE clause regardless of
-- whether it fired); the last 2 indexes are always this observing SELECT's
-- own leading-in-time-but-trailing-in-the-array QueryStart, ExecStart
-- (written to the ring before its body/aggregate runs).  Slicing them off
-- leaves exactly the case's own real, non-Idle markers.  Real wait events
-- are deliberately never asserted by exact count (plan sec 5.5): the one
-- case with a real wait (case 7) checks only presence/attribution.
--
CREATE EXTENSION IF NOT EXISTS pg_wait_event_tracing;

-- CI forces debug_parallel_query = regress on some platforms, which
-- would move statements below into a parallel worker, recording their
-- markers under the worker's own ring, not this session's.
SET debug_parallel_query = off;

SET pg_wait_event_tracing.capture = trace;

--
-- Case 1: a single autocommit statement.
-- Expect: QueryStart, ExecStart, ExecEnd, TxnCommit (Idle excluded; it
-- would follow once the implicit transaction has committed and the
-- backend waits for the next client message -- see the file header).
--
SELECT coalesce(max(seq), -1) AS mark1 FROM pg_backend_wait_event_trace \gset
SELECT 1;
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark1;

--
-- Case 2: an explicit transaction with two statements, each on its own
-- line.  The transaction stays open the whole time (no TxnCommit/TxnAbort
-- until the final COMMIT).  TxnCommit follows UtilityEnd here, the same
-- as case 4's plain utility statement, not during ProcessUtility: an
-- explicit COMMIT's EndTransactionBlock() only marks the transaction
-- block TBLOCK_END while still inside the utility statement: the actual
-- commit (CommitTransaction(), which fires the xact callback) happens in
-- finish_xact_command(), called by exec_simple_query() (postgres.c)
-- *after* ProcessUtility returns -- the same command-loop point an
-- ordinary autocommit statement's implicit commit happens at.
--
SELECT coalesce(max(seq), -1) AS mark2 FROM pg_backend_wait_event_trace \gset
BEGIN;
SELECT 1;
SELECT 2;
COMMIT;
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark2;

--
-- Case 3: two statements sent as ONE simple-query protocol message, both
-- on the same input line so psql sends them together.  Each still gets
-- its own individual QueryStart/ExecStart/ExecEnd/TxnCommit -- autocommit
-- commits after every statement in a multi-statement string, not once at
-- the end.
--
SELECT coalesce(max(seq), -1) AS mark3 FROM pg_backend_wait_event_trace \gset
SELECT 1; SELECT 2;
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark3;

--
-- Case 4: a utility statement (no executor involvement): UtilityStart/
-- UtilityEnd only, no ExecStart/ExecEnd, TxnCommit after UtilityEnd --
-- same shape and same reason as case 2's COMMIT above (the implicit
-- per-statement commit happens in the command loop, after
-- ProcessUtility_hook returns).
--
SELECT coalesce(max(seq), -1) AS mark4 FROM pg_backend_wait_event_trace \gset
CREATE TABLE pwet_trace_test_t (a int);
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark4;
DROP TABLE pwet_trace_test_t;

--
-- Case 5: an error raised during PLANNING, not execution: 1/0 is a
-- constant expression, and the planner's eval_const_expressions() folds
-- it by calling evaluate_expr() (clauses.c), which builds a throwaway
-- executor state and evaluates the expression right there -- raising the
-- division-by-zero before ExecutorStart is ever reached.  So there is no
-- ExecStart/ExecEnd pair to leave unmatched here: QueryStart opens the
-- statement's interval, and TxnAbort -- not TxnCommit -- closes it
-- directly.  This no longer exercises pwet_marker_txn_abort()'s
-- defensive pwet_exec_depth reset (needed for a genuinely mid-execution
-- error, which this simple, always-planning-time-erroring form cannot
-- produce); that reset stays uncovered by this regression file.
--
SELECT coalesce(max(seq), -1) AS mark5 FROM pg_backend_wait_event_trace \gset
SELECT 1/0;
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark5;

--
-- Case 6: a nested query call (depth 1 inside depth 0).  LANGUAGE SQL
-- will not do here: inline_function() (clauses.c) inlines a SQL-language
-- function whose body is a single simple SELECT directly into the
-- calling query, so it never runs its own separate, nested executor
-- invocation at all.  PL/pgSQL is not inlined, and a PERFORM statement
-- in its body always goes through SPI's normal execute path (unlike a
-- bare RETURN/assignment expression, which plpgsql evaluates directly
-- without SPI when it is "simple enough"), so it reliably produces a
-- real nested ExecStart/ExecEnd pair.  plpgsql is installed in every
-- database by default, so this needs no extra CREATE EXTENSION.
--
-- The function is called once first, outside the measured window, so
-- its PERFORM statement's query is already parsed and its plan cached
-- (plpgsql caches each statement's plan across calls in the same
-- session) by the time of the measured call -- keeping this case about
-- the ExecStart/ExecEnd depth nesting specifically, not about whether a
-- cached call also re-parses (it does not, so no QueryStart happens at
-- nested depth in the measured call).
--
CREATE FUNCTION pwet_trace_test_nested() RETURNS int
LANGUAGE plpgsql AS $$ BEGIN PERFORM 1; RETURN 1; END $$;
SELECT pwet_trace_test_nested();

SELECT coalesce(max(seq), -1) AS mark6 FROM pg_backend_wait_event_trace \gset
SELECT pwet_trace_test_nested();
SELECT (array_agg(wait_event ORDER BY seq))[3:count(*)-2] AS markers,
       (array_agg(depth ORDER BY seq))[3:count(*)-2] AS depths
FROM pg_backend_wait_event_trace
WHERE wait_event_type = 'Query' AND wait_event <> 'Idle' AND seq > :mark6;
DROP FUNCTION pwet_trace_test_nested();

--
-- Case 7: pg_wait_event_trace_by_statement() attribution.  A pg_sleep()
-- inside its own statement guarantees at least one real (Timeout/
-- PgSleep) wait to attribute; the following bare SELECT has none.  Never
-- assert an exact PgSleep count (plan sec 5.5): only that every PgSleep
-- wait this session ever recorded was attributed to a real statement,
-- never to the synthetic <idle>/<unattributed> buckets, and that at
-- least one such wait was seen.
--
SELECT procnumber FROM pg_stat_wait_event_timing
WHERE pid = pg_backend_pid() LIMIT 1 \gset

SELECT pg_sleep(0.01);
SELECT 1;

SELECT bool_and(bucket NOT IN ('<idle>', '<unattributed>')) AS pgsleep_attributed,
       sum(calls) >= 1 AS at_least_one_pgsleep
FROM pg_wait_event_trace_by_statement(:procnumber)
WHERE wait_event = 'PgSleep';

RESET pg_wait_event_tracing.capture;
