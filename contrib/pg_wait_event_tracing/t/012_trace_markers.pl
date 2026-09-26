# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: two marker-set corners the module's own regress
# test (sql/pg_wait_event_tracing_trace.sql) deliberately does not cover,
# because a deterministic .sql script cannot produce either hazard on
# demand:
#
#   (a) the Idle marker.  pwet_wait_begin() only synthesizes it when a
#       ClientRead wait actually blocks -- and secure_read() (be-secure.c)
#       only reports WAIT_EVENT_CLIENT_READ from the branch taken after a
#       non-blocking read returns EWOULDBLOCK, never around a read that is
#       immediately satisfied from already-buffered client bytes.  Whether
#       that happens for two statements sent from a .sql file depends on
#       loaded-runner scheduling, not protocol structure, so the regress
#       test filters Idle out of every case entirely and documents this
#       exact deferral.  A TAP test controls the client side directly: a
#       real pause between two statements must produce an Idle marker
#       between them.
#
#       The opposite check -- two statements that share ONE simple-query
#       protocol message never see an Idle between them -- needs an
#       actual single message, not just two statements on one input
#       line: CI showed that psql, reading a script from a file (or
#       $node->safe_psql's string), sends each ;-terminated statement as
#       its own message regardless of shared line placement, so "two
#       statements, one line" was exactly as timing-dependent as the
#       thing being tested, and failed on Windows/macOS while passing on
#       Linux. `psql -c 'SELECT 1; SELECT 2;'` does send the whole
#       string as one message; see the case's own comment below for the
#       exact marker sequence that message produces and how its
#       identity (needed to read the ring back afterward) is captured
#       without perturbing it. Also note psql's own documented rule
#       (psql-ref.sgml, "-c"/"-f"): once any -c or -f is given, psql
#       never reads a script from standard input at all, so the target
#       message and everything needed to identify its session must all
#       be passed as -c arguments -- nothing can be layered in via the
#       piped script $node->psql() would otherwise send.
#
#   (b) pwet_marker_txn_abort()'s defensive pwet_exec_depth reset.  The
#       regress test's own error case (SELECT 1/0) raises at PLANNING
#       time -- eval_const_expressions() folds the constant division
#       before ExecutorStart is ever reached -- so pwet_exec_depth never
#       moves and the reset is never exercised.  Here, a PL/pgSQL PERFORM
#       divides by a column value that is only zero on the second row of
#       a table scan, so the division-by-zero can only be discovered
#       while genuinely executing that nested statement.  The call is
#       wrapped in a PROCEDURE, not a plain function: CALL is dispatched
#       through ProcessUtility (T_CallStmt in standard_ProcessUtility),
#       not the executor, so the outer call contributes a UtilityStart
#       marker without ever touching pwet_exec_depth, and exactly ONE
#       nested executor level -- the PERFORM's own -- is left unclosed by
#       the error.  Without the reset, every later statement's own
#       ExecStart would be off by exactly that one level.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');

# ---------------------------------------------------------------------
# (a) Idle marker: present after a real client-side pause between two
# statements, absent between two statements sent as one simple-query
# message.
# ---------------------------------------------------------------------
my $psql = $node->background_psql('postgres');
$psql->query_safe('SET pg_wait_event_tracing.capture = trace;');

my $mark_a = $psql->query_safe(
	"SELECT coalesce(max(seq), -1) FROM pg_backend_wait_event_trace;");
$psql->query_safe('SELECT 1;');

# A real, generous client-side pause: by the time this session next
# tries to read the following message, nothing has arrived yet, so
# secure_read() genuinely blocks in WaitEventSetWait(WAIT_EVENT_CLIENT_
# READ) -- the only place the Idle marker is synthesized (see the file
# header).  A couple of seconds is comfortably more than any scheduling
# jitter on a loaded CI runner needs to be sure of that.
sleep(2);
$psql->query_safe('SELECT 2;');

my $markers_a = $psql->query_safe(
	"SELECT string_agg(wait_event, ',' ORDER BY seq) "
	  . "FROM pg_backend_wait_event_trace "
	  . "WHERE wait_event_type = 'Query' AND seq > $mark_a;");
like($markers_a, qr/(^|,)Idle(,|$)/,
	'a real client-side pause between two statements produces an Idle marker'
);

$psql->quit;

# Everything runs as -c arguments, never a piped script (see the file
# header for why the latter cannot be mixed with -c at all).  Multiple
# -c's are fine -- each is processed in turn, in one connection/session
# -- so the pid/procnumber lookups run as their own two single-statement
# messages, BEFORE the target message enables capture, and so add zero
# markers to this session's ring (pwet_trace_write_marker() is a no-op
# outside capture = trace).  That leaves the ring holding nothing but
# the target message's own markers, so no anchor/window bookkeeping is
# needed to isolate them from anything earlier; LIMIT 8 below only
# guards against a possible trailing Idle marker from the session's own
# eventual exit (belt-and-braces, not otherwise relied on).
#
# The target message is "SET pg_wait_event_tracing.capture = trace;
# SELECT 1; SELECT 2;" -- three statements, one message, so it can never
# see a ClientRead wait (and so no Idle) between any of them.  The SET's
# effect (verified by reading exec_simple_query() in postgres.c
# directly) is visible to the later statements in the SAME message: a
# GUC assign hook runs synchronously as part of executing the SET, well
# before the message is done.  But because more than one statement
# shares this message, postgres.c wraps the whole thing in one implicit
# transaction block that commits only once, when the LAST statement
# (SELECT 2) finishes -- not once per statement, unlike two statements
# each sent as their own separate message (unlike case 3 of the
# module's own regress test).  So the expected marker sequence is:
# UtilityEnd for the SET (its UtilityStart is skipped: capture is still
# off when that check runs, before the SET's own assign hook has fired),
# then QueryStart/ExecStart/ExecEnd for SELECT 1 and again for SELECT 2
# with no TxnCommit of their own (mid-block statements only get a
# CommandCounterIncrement, not a real commit), and finally one TxnCommit
# -- for the whole block -- once SELECT 2 closes it.  Eight markers,
# none of them Idle, since nothing in this design ever gives the backend
# a reason to attempt a read before the message is fully processed.
#
# The session then exits (a one-shot $node->psql call, not a persistent
# BackgroundPsql session), orphaning its ring (same mechanism as
# t/005_orphan_reuse.pl), which is read back cross-backend once the
# backend is confirmed gone.
my (undef, $case2_out, undef) = $node->psql(
	'postgres', '',
	on_error_die => 1,
	extra_params => [
		'-c', 'SELECT pg_backend_pid();',
		'-c', 'SELECT id FROM pg_stat_get_backend_idset() AS id '
		  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();',
		'-c', 'SET pg_wait_event_tracing.capture = trace; '
		  . 'SELECT 1; SELECT 2;',
	]);
my ($case2_pid, $case2_procnumber) = split /\n/, $case2_out;

$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $case2_pid);"
) or die "backend $case2_pid did not disappear from pg_stat_activity";

my $markers_b = $node->safe_psql(
	'postgres', qq(
	SELECT string_agg(wait_event, ',' ORDER BY seq) FROM (
	    SELECT wait_event, seq
	    FROM pg_get_wait_event_trace($case2_procnumber)
	    WHERE wait_event_type = 'Query'
	    ORDER BY seq
	    LIMIT 8
	) t;
));
is( $markers_b,
	'UtilityEnd,QueryStart,ExecStart,ExecEnd,QueryStart,ExecStart,ExecEnd,TxnCommit',
	'the SET+SELECT1+SELECT2 single message produces exactly its own '
	  . 'eight markers, with no Idle between any of them');

# ---------------------------------------------------------------------
# (b) pwet_marker_txn_abort()'s defensive pwet_exec_depth reset, after an
# error raised during execution (not planning), inside a nested call.
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE TABLE pwet_trace_divzero_rows (d int);
INSERT INTO pwet_trace_divzero_rows VALUES (1), (0);
CREATE PROCEDURE pwet_trace_test_divzero() LANGUAGE plpgsql AS $body$
DECLARE
    r record;
BEGIN
    FOR r IN SELECT d FROM pwet_trace_divzero_rows ORDER BY d DESC LOOP
        PERFORM 1 / r.d;
    END LOOP;
END
$body$;
));

# on_error_stop => 0: the CALL below is expected to fail, and the same
# session must survive it to run a following statement.  A check that
# expects an ERROR must not use a plain background_psql session (its
# default on_error_stop would make psql exit on the error, and the next
# call into this session would die with "process ended prematurely").
my $psql2 = $node->background_psql('postgres', on_error_stop => 0);
$psql2->query_safe('SET pg_wait_event_tracing.capture = trace;');

$psql2->query('CALL pwet_trace_test_divzero();');
like($psql2->{stderr}, qr/division by zero/,
	'the PERFORM divides by zero on the second row, mid-execution');
$psql2->{stderr} = '';

# The next, ordinary statement's own ExecStart marker is self-
# referential (same as the regress test: post_parse_analyze/
# ExecutorStart write this SELECT's own QueryStart/ExecStart before its
# body runs), so its depth field reports the nesting level in effect
# right after the abort.  Without pwet_marker_txn_abort()'s reset, the
# PERFORM's own ExecStart -- never matched by an ExecEnd, since the
# error struck mid-execution -- would leave pwet_exec_depth stuck at 1
# forever.
my $depth = $psql2->query_safe(
	"SELECT depth FROM pg_backend_wait_event_trace "
	  . "WHERE wait_event = 'ExecStart' ORDER BY seq DESC LIMIT 1;");
is($depth, '0',
	"a normal statement's ExecStart depth is back at 0 after the aborted CALL"
);

$psql2->quit;
$node->stop;

done_testing();
