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
#   (b) the executor depth after an error raised during execution.  The
#       regress test's own error case (SELECT 1/0) raises at PLANNING
#       time -- eval_const_expressions() folds the constant division
#       before ExecutorStart is ever reached -- so no executor call is
#       unwound.  Here, a PL/pgSQL PERFORM divides by a column value that
#       is only zero on the second row of a table scan, so the
#       division-by-zero is raised inside the nested statement's
#       ExecutorRun.  The depth counts the executor hook calls on the
#       stack (their frames on error_context_stack, which the error
#       recovery unwinds), so the error must leave it at 0 for the next
#       statement; ExecEnd is never written for the failed statement.
#
#   (c) IdleInTransaction: the Idle marker of a pause inside an open
#       transaction.
#
#   (d) the utility nesting depth carried by TxnCommit/TxnAbort, and the
#       executor depth after a caught execution error or a DECLARE CURSOR
#       whose executor stays open.
#
#   (e) a backend terminated inside a CALL, after it committed a temporary
#       table: the temporary-table cleanup at exit aborts and commits
#       transactions while the CALL was never unwound, and those markers
#       must still say depth 0.
#
#   (f) a parallel worker running a utility statement (SHOW, via a
#       PARALLEL SAFE function): its own ring, its transaction end at depth
#       0, no Idle marker.
#
#   (g) COPY FROM STDIN with a trigger, its data sent in two chunks: the
#       trigger's nested statement ends, then COPY waits for the next chunk
#       in ClientRead; that wait is inside a statement and must not produce
#       an Idle marker.
#
#   (h) cursors, whose executor outlives the statement that started it: a
#       cursor declared and closed inside a function, a WITH HOLD cursor
#       run to completion at commit, and a FETCH; the SQL they run is
#       nested in the statement running them.  Also SQL a stable function
#       runs while a generic plan is made (an estimate) and while it is
#       started (initial partition pruning).
#
#   (i) a portal suspended by an Execute with a row limit, whose function
#       ran a utility statement (SHOW), then a pause, then a second Execute:
#       the pause must not end the statement, and the second Execute's
#       waits stay in its bucket.  psql cannot send a row limit, so this
#       speaks the protocol itself, over the Unix-domain socket.
#
#   (j) the extension in a schema off the search path.
#
#   (k) the depth when capture changes in the middle of things: switched
#       to trace inside a function, inside a CALL that commits, in a
#       transaction after a failed statement in a savepoint, with a WITH
#       HOLD cursor or a suspended portal already open, switched off and on
#       again inside a function, and after errors caught while capture was
#       off (PL/pgSQL, PL/Perl, PL/Python, when available).  The depth is
#       counted from frames that every hook puts on error_context_stack
#       whatever the capture level, so the first marker after the switch
#       already has the right depth.
#
#   (l) the frames' error context callback is silent: messages, their
#       CONTEXT lines and GET DIAGNOSTICS ... PG_CONTEXT are the same as on
#       a server without the module.
#
#   (m) parallel query with capture = trace: Gather run to the end, under
#       LIMIT, in a cursor and in a portal fetched in pieces, with
#       notices from the workers (relayed with the error context stack
#       saved when the parallel context was created) and with an error
#       caught by an EXCEPTION block and one that ends the statement.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

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
# (b) the executor depth after an error raised during execution (not
# planning), inside a nested call.
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
# right after the abort: 0, although the PERFORM's ExecStart was never
# matched by an ExecEnd.
my $depth = $psql2->query_safe(
	"SELECT depth FROM pg_backend_wait_event_trace "
	  . "WHERE wait_event = 'ExecStart' ORDER BY seq DESC LIMIT 1;");
is($depth, '0',
	"a normal statement's ExecStart depth is back at 0 after the aborted CALL"
);

$psql2->quit;

# ---------------------------------------------------------------------
# Helpers for (c)-(g): another session reads the ring of the session under
# test, so reading adds no markers to it.
# ---------------------------------------------------------------------

# Run $sql in BackgroundPsql session $s and return its stderr.  Unlike
# query(), sends nothing but $sql: query() adds an empty query, which commits
# an empty transaction and so writes a TxnCommit of its own.
my $run_cnt = 0;

sub run_sql
{
	my ($s, $sql) = @_;
	my $token = 'PWET_DONE_' . ++$run_cnt;
	$s->{stdout} = '';
	$s->{stderr} = '';
	$s->{stdin} .= "$sql\n\\echo $token\n\\warn $token\n";
	# Wait for the token AND its line end: psql writes \echo/\warn text and
	# its newline separately (stderr is unbuffered), so stopping at the bare
	# token could leave the newline to show up in the next call's stderr.
	# \r\n on Windows.
	pump_until($s->{run}, $s->{timeout}, \$s->{stdout}, qr/$token\r?\n/)
	  or die "psql did not finish \"$sql\": stdout: $s->{stdout}";
	pump_until($s->{run}, $s->{timeout}, \$s->{stderr}, qr/$token\r?\n/)
	  or die "psql did not finish \"$sql\": stderr: $s->{stderr}";
	my $err = $s->{stderr};
	$err =~ s/^.*$token.*\r?\n//mg;
	$s->{stdout} = '';
	$s->{stderr} = '';
	return $err;
}

# pid and procnumber of a BackgroundPsql session
sub ids_of
{
	my ($s) = @_;
	my $pid = $s->query_safe('SELECT pg_backend_pid();');
	my $pn = $s->query_safe(
		'SELECT id FROM pg_stat_get_backend_idset() AS id '
		  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();');
	chomp($pid);
	chomp($pn);
	return ($pid, $pn);
}

sub ring_mark
{
	my ($pn) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(max(seq), -1) FROM pg_get_wait_event_trace($pn);");
}

# For a failed check that needs a recorded pg_sleep() wait: the raw number
# of PgSleep records of ring $pn after seq $after, so that a pg_sleep() that
# recorded no wait (its backend stalled for longer than the sleep between
# computing its deadline and testing it) can be told from an attribution
# bug.  Only called from diag(), on failure.
sub raw_sleeps
{
	my ($pn, $after) = @_;
	return "raw PgSleep records in ring $pn after seq $after: "
	  . $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($pn) "
		  . "WHERE wait_event = 'PgSleep' AND seq > $after;");
}

# The newest seq of ring $pn once its session has gone idle: wait until the
# newest marker is Idle or IdleInTransaction first.  run_sql() and query()
# return when psql has the result, before the backend has necessarily
# blocked in ClientRead and written the Idle marker of the statement just
# run; a mark taken earlier could have that Idle land after it.  Only for a
# session that traces and has run a statement since its last idle marker.
sub idle_mark
{
	my ($pn) = @_;
	$node->poll_query_until(
		'postgres', qq(
		SELECT wait_event IN ('Idle', 'IdleInTransaction')
		FROM pg_get_wait_event_trace($pn)
		WHERE wait_event_type = 'Query'
		ORDER BY seq DESC LIMIT 1;
	)) or die "ring $pn: the session did not go idle";
	return ring_mark($pn);
}

# Wait until the newest marker of ring $pn is $kind and newer than $after.
sub wait_for_marker
{
	my ($pn, $kind, $after) = @_;
	$node->poll_query_until(
		'postgres', qq(
		SELECT wait_event = '$kind' AND seq > $after
		FROM pg_get_wait_event_trace($pn)
		WHERE wait_event_type = 'Query'
		ORDER BY seq DESC LIMIT 1;
	)) or die "ring $pn: no $kind marker after seq $after";
}

# The markers of ring $pn after seq $after, as "Event" or "Event:depth"
# (with $with_depth), comma-separated; only those named in @only, if given.
sub markers_after
{
	my ($pn, $after, $with_depth, @only) = @_;
	my $filter = @only
	  ? 'AND wait_event IN (' . join(', ', map { "'$_'" } @only) . ')'
	  : '';
	my $suffix = $with_depth ? "':' || depth" : "''";
	return $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(wait_event || $suffix, ',' ORDER BY seq)
		FROM pg_get_wait_event_trace($pn)
		WHERE wait_event_type = 'Query' AND seq > $after $filter;
	));
}

# ---------------------------------------------------------------------
# (c) IdleInTransaction
# ---------------------------------------------------------------------
my $psql3 = $node->background_psql('postgres', on_error_stop => 0);
$psql3->query_safe('SET pg_wait_event_tracing.capture = trace;');
my ($pid3, $pn3) = ids_of($psql3);

my $mark = idle_mark($pn3);
run_sql($psql3, 'BEGIN;');
run_sql($psql3, 'SELECT 1;');
wait_for_marker($pn3, 'IdleInTransaction', $mark);
run_sql($psql3, 'COMMIT;');
wait_for_marker($pn3, 'Idle', $mark);
like(
	markers_after($pn3, $mark, 0),
	qr/QueryStart,ExecStart,ExecEnd,IdleInTransaction,QueryStart,UtilityStart,UtilityEnd,TxnCommit,Idle$/,
	'a pause inside a transaction block writes IdleInTransaction, '
	  . 'the pause after its COMMIT Idle');

# an aborted transaction block can no longer commit: Idle, not
# IdleInTransaction
$mark = idle_mark($pn3);
run_sql($psql3, 'BEGIN;');
# the backend must have waited for input before the next statement arrives
wait_for_marker($pn3, 'IdleInTransaction', $mark);
like(run_sql($psql3, 'SELECT 1 / 0;'), qr/division by zero/,
	'the transaction block fails');
wait_for_marker($pn3, 'Idle', $mark);
run_sql($psql3, 'ROLLBACK;');
wait_for_marker($pn3, 'Idle', $mark);
like(
	markers_after($pn3, $mark, 0),
	qr/IdleInTransaction,QueryStart,TxnAbort,Idle,QueryStart,UtilityStart,UtilityEnd,Idle$/,
	'a pause in an aborted transaction block writes Idle');

# ---------------------------------------------------------------------
# (d) utility depth on TxnCommit/TxnAbort, executor depth after a utility
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE PROCEDURE pwet_trace_rb() LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM 1;
    ROLLBACK;
    PERFORM 1;
END
$body$;
CREATE PROCEDURE pwet_trace_commit() LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM 1;
    COMMIT;
    PERFORM 1;
END
$body$;
CREATE PROCEDURE pwet_trace_outer() LANGUAGE plpgsql AS $body$
BEGIN
    CALL pwet_trace_commit();
END
$body$;
CREATE PROCEDURE pwet_trace_rb_err() LANGUAGE plpgsql AS $body$
BEGIN
    ROLLBACK;
    PERFORM 1 / d FROM pwet_trace_divzero_rows;
END
$body$;
CREATE PROCEDURE pwet_trace_caught() LANGUAGE plpgsql AS $body$
BEGIN
    BEGIN
        PERFORM 1 / d FROM pwet_trace_divzero_rows;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
END
$body$;
));

my @txn = ('TxnCommit', 'TxnAbort');
foreach my $case (
	[ 'CALL pwet_trace_rb();', 'TxnAbort:1,TxnCommit:0',
		'a ROLLBACK inside a procedure: TxnAbort depth 1, the CALL\'s own commit depth 0' ],
	[ 'CALL pwet_trace_commit();', 'TxnCommit:1,TxnCommit:0',
		'a COMMIT inside a procedure: TxnCommit depth 1, then depth 0' ],
	[ 'CALL pwet_trace_outer();', 'TxnCommit:2,TxnCommit:0',
		'a COMMIT inside a nested CALL: depth 2' ],
	[ 'CALL pwet_trace_rb_err();', 'TxnAbort:1,TxnAbort:0',
		'a ROLLBACK, then an error escaping the CALL: depth 1, then 0' ])
{
	my ($sql, $want, $name) = @$case;
	$mark = idle_mark($pn3);
	run_sql($psql3, $sql);
	wait_for_marker($pn3, 'Idle', $mark);
	is(markers_after($pn3, $mark, 1, @txn), $want, $name);
}

# the failed CALL of (b): UtilityEnd is written on error, before TxnAbort
$mark = idle_mark($pn3);
like(run_sql($psql3, 'CALL pwet_trace_test_divzero();'),
	qr/division by zero/, 'the CALL fails');
wait_for_marker($pn3, 'Idle', $mark);
like(markers_after($pn3, $mark, 1, 'UtilityStart', 'UtilityEnd', @txn),
	qr/^UtilityStart:0,UtilityEnd:0,TxnAbort:0$/,
	'an error escaping a CALL writes UtilityEnd, then TxnAbort depth 0');

# Inside one transaction (no transaction end in between), a CALL that
# caught an execution error, and a DECLARE CURSOR whose executor stays
# open, leave the next statement at depth 0.
foreach my $case ([ 'CALL pwet_trace_caught();', 'a CALL that caught an execution error' ],
	[ 'DECLARE pwet_c CURSOR FOR SELECT 1;', 'DECLARE CURSOR' ])
{
	my ($sql, $name) = @$case;
	$mark = idle_mark($pn3);
	run_sql($psql3, "BEGIN;\n$sql\nSELECT 2;\nCOMMIT;");
	wait_for_marker($pn3, 'Idle', $mark);
	my $depth = $node->safe_psql(
		'postgres', qq(
		SELECT depth FROM pg_get_wait_event_trace($pn3)
		WHERE wait_event = 'ExecStart' AND seq > $mark
		ORDER BY seq DESC LIMIT 1;
	));
	is($depth, '0', "$name, then SELECT in the same transaction: depth 0");
}
$psql3->quit;

# ---------------------------------------------------------------------
# (e) termination inside a CALL, after a temporary table was committed
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE PROCEDURE pwet_trace_long() LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM pg_sleep(300);
END
$body$;
));
my $psql4 = $node->background_psql('postgres');
# capture first: the module registers its exit callback when it first
# attaches, before the temporary table's cleanup callback, so the cleanup
# runs first at exit and its transactions reach the ring
$psql4->query_safe('SET pg_wait_event_tracing.capture = trace;');
$psql4->query_safe('SELECT 1;');
$psql4->query_safe('CREATE TEMP TABLE pwet_trace_tmp (a int);');
my ($pid4, $pn4) = ids_of($psql4);
$mark = idle_mark($pn4);
$psql4->query_until(qr/calling/, "\\echo calling\nCALL pwet_trace_long();\n");
$node->poll_query_until(
	'postgres', qq(
	SELECT wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid = $pid4;
)) or die "backend $pid4 never slept inside the CALL";
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid4);");
$node->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $pid4);")
  or die "backend $pid4 did not exit";
# psql has lost its connection; quit() can fail on the dead pipe.  Say so
# instead of hiding it.
eval { $psql4->quit; 1 } or note("quit of the terminated session: $@");

is(markers_after($pn4, $mark, 1, 'UtilityStart', 'UtilityEnd', @txn),
	'UtilityStart:0,TxnAbort:0,TxnCommit:0',
	'terminated inside a CALL: the exit abort and the temporary-table '
	  . 'cleanup commit say depth 0');
# pg_sleep() can complete more than one wait (a latch wakeup restarts it),
# so the bucket must hold exactly the PgSleep records the ring has.
my ($sleep_buckets, $sleep_calls, $sleep_raw) = split /\|/,
  $node->safe_psql(
	'postgres', qq(
	SELECT count(DISTINCT statement_seq) || '|' || coalesce(sum(calls), 0)
	       || '|' || (SELECT count(*) FROM pg_get_wait_event_trace($pn4)
	                  WHERE seq > $mark AND wait_event = 'PgSleep')
	FROM pg_wait_event_trace_by_statement($pn4)
	WHERE wait_event = 'PgSleep' AND statement_seq > $mark;
));
is($sleep_buckets, 1, '...the interrupted sleep is in one bucket');
cmp_ok($sleep_calls, '>=', 1, '...which holds at least one PgSleep wait');
is($sleep_calls, $sleep_raw, '...and every PgSleep wait the CALL recorded');

# ---------------------------------------------------------------------
# (f) a parallel worker running a utility statement
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_trace_show() RETURNS text
LANGUAGE plpgsql VOLATILE PARALLEL SAFE AS $body$
DECLARE
    v text;
BEGIN
    EXECUTE 'SHOW work_mem' INTO v;
    RETURN v;
END
$body$;
SELECT pg_stat_clear_orphaned_wait_event_rings();
));
my $psql5 = $node->background_psql('postgres');
$psql5->query_safe('SET pg_wait_event_tracing.capture = trace;');
my ($pid5, $pn5) = ids_of($psql5);
$psql5->query_safe('SET debug_parallel_query = on;');
like($psql5->query_safe('EXPLAIN (COSTS OFF) SELECT pwet_trace_show();'),
	qr/Gather/, 'the function runs under a Gather');
$psql5->query_safe('SELECT pwet_trace_show();');
$psql5->quit;

# the worker's ring: a ring of another process that saw the SHOW
my $worker_rings = qq(
	SELECT pn, owner_pid FROM generate_series(0, 1000) pn,
	     LATERAL pg_get_wait_event_trace(pn) t
	WHERE owner_pid <> $pid5 AND wait_event = 'UtilityStart');
$node->poll_query_until('postgres',
	"SELECT EXISTS ($worker_rings);")
  or die 'no parallel worker ring with a UtilityStart';
my ($wpn) = split /\|/,
  $node->safe_psql('postgres', "$worker_rings LIMIT 1;");
my $wmarkers = markers_after($wpn, -1, 1);
like($wmarkers, qr/UtilityStart:([1-9]\d*),UtilityEnd:\1/,
	'parallel worker: SHOW wrote UtilityStart and UtilityEnd');
like($wmarkers, qr/TxnCommit:0$/,
	'parallel worker: its transaction ends at depth 0');
unlike($wmarkers, qr/Idle|TxnCommit:[1-9]|TxnAbort:[1-9]/,
	'parallel worker: no Idle marker, no transaction end inside a utility')
  or diag($wmarkers);

# ---------------------------------------------------------------------
# (g) COPY FROM STDIN with a trigger, data in two chunks
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE TABLE pwet_trace_copy (a int);
CREATE FUNCTION pwet_trace_copy_trg() RETURNS trigger
LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM pg_sleep(0.1);
    RETURN NEW;
END
$body$;
CREATE TRIGGER pwet_trace_copy_trg BEFORE INSERT ON pwet_trace_copy
    FOR EACH ROW WHEN (NEW.a = 1) EXECUTE FUNCTION pwet_trace_copy_trg();
));
my $psql6 = $node->background_psql('postgres');
$psql6->query_safe('SET pg_wait_event_tracing.capture = trace;');
my ($pid6, $pn6) = ids_of($psql6);
$mark = idle_mark($pn6);

# The first chunk: the one row that fires the trigger, then enough rows that
# psql and libpq, which buffer COPY data in 8 kB pieces, send most of it.
# The rest stays in psql until the second chunk arrives, so the backend
# processes the first row and then waits in ClientRead inside the COPY.
$psql6->{stdin} .= "COPY pwet_trace_copy FROM STDIN;\n1\n" . ("0\n" x 20000);
# hand the whole chunk to psql: one nonblocking pump may write only part of
# it, and psql sends nothing before it has a full buffer
my $copy_deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
while ($psql6->{stdin} ne '')
{
	die 'psql did not take the first COPY chunk' if time() > $copy_deadline;
	$psql6->{run}->pump_nb;
	usleep(10_000) if $psql6->{stdin} ne '';
}
$node->poll_query_until(
	'postgres', qq(
	SELECT EXISTS (SELECT 1 FROM pg_get_wait_event_trace($pn6)
	               WHERE seq > $mark AND wait_event = 'ExecEnd')
	   AND (SELECT state = 'active' AND wait_event = 'ClientRead'
	        FROM pg_stat_activity WHERE pid = $pid6);
)) or die "backend $pid6 never waited for COPY data after the trigger ran";
is(run_sql($psql6, ("0\n" x 10) . "\\."), '', 'COPY succeeded');
wait_for_marker($pn6, 'Idle', $mark);

my ($copy_qs, $copy_ue, $trg_ee) = split /\|/,
  $node->safe_psql(
	'postgres', qq(
	SELECT min(seq) FILTER (WHERE wait_event = 'QueryStart') || '|'
	    || min(seq) FILTER (WHERE wait_event = 'UtilityEnd') || '|'
	    || min(seq) FILTER (WHERE wait_event = 'ExecEnd')
	FROM pg_get_wait_event_trace($pn6) WHERE seq > $mark;
));
cmp_ok(
	$node->safe_psql(
		'postgres', qq(
		SELECT count(*) FROM pg_get_wait_event_trace($pn6)
		WHERE seq > $trg_ee AND seq < $copy_ue AND wait_event = 'ClientRead';
	)),
	'>=', 1,
	'COPY waited in ClientRead after the trigger\'s statement ended');
is( markers_after($pn6, $copy_qs, 0, 'Idle', 'IdleInTransaction',
		'UtilityEnd'),
	'UtilityEnd,Idle',
	'...and no Idle marker was written inside the COPY');
is( $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(DISTINCT statement_seq::text, ',')
		FROM pg_wait_event_trace_by_statement($pn6)
		WHERE wait_event = 'PgSleep' AND statement_seq > $mark;
	)),
	$copy_qs,
	'the trigger\'s sleep is in the COPY\'s bucket')
  or diag(raw_sleeps($pn6, $mark));
$psql6->quit;

# ---------------------------------------------------------------------
# (h) cursors: an executor that outlives the statement that started it
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_trace_cursor() RETURNS void LANGUAGE plpgsql AS $body$
DECLARE
    c refcursor := 'pwet_trace_c';
BEGIN
    EXECUTE 'DECLARE pwet_trace_c CURSOR FOR SELECT 1';
    CLOSE c;
    PERFORM pg_sleep(0.1) FROM generate_series(1, 1);
END
$body$;
CREATE FUNCTION pwet_trace_sleepy(i int) RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM pg_sleep(0.1);
    RETURN i;
END
$body$;
));
my $psql7 = $node->background_psql('postgres');
$psql7->query_safe('SET pg_wait_event_tracing.capture = trace;');
my ($pid7, $pn7) = ids_of($psql7);

# Run $sql in session 7 and check that every PgSleep wait it caused is in
# one bucket, opened by the $nth depth-0 QueryStart of $sql.  DECLARE
# CURSOR writes two of them: its own and its query's, which it parses
# inside the utility statement.
sub check_sleep_bucket
{
	my ($sql, $nth, $min_calls, $name) = @_;
	my $m = idle_mark($pn7);
	is(run_sql($psql7, $sql), '', "$name: ran");
	wait_for_marker($pn7, 'Idle', $m);
	my $qs = $node->safe_psql(
		'postgres', qq(
		SELECT seq FROM pg_get_wait_event_trace($pn7)
		WHERE seq > $m AND wait_event = 'QueryStart' AND depth = 0
		ORDER BY seq OFFSET $nth - 1 LIMIT 1;
	));
	my ($buckets, $calls, $raw) = split /\|/,
	  $node->safe_psql(
		'postgres', qq(
		SELECT coalesce(string_agg(DISTINCT bucket, ','), '') || '|'
		       || coalesce(sum(calls), 0) || '|'
		       || (SELECT count(*) FROM pg_get_wait_event_trace($pn7)
		           WHERE seq > $m AND wait_event = 'PgSleep')
		FROM pg_wait_event_trace_by_statement($pn7)
		WHERE wait_event = 'PgSleep'
		  AND (statement_seq > $m OR statement_seq IS NULL);
	));
	is($buckets, $qs, "$name: the sleeps are in the bucket of statement $nth")
	  or diag(markers_after($pn7, $m, 1), "\n", raw_sleeps($pn7, $m));
	cmp_ok($calls, '>=', $min_calls, "$name: at least $min_calls PgSleep waits")
	  or diag(raw_sleeps($pn7, $m));
	is($calls, $raw, "$name: all of them");
}

# DECLARE inside a function, CLOSE from PL/pgSQL (no utility statement):
# the PERFORM after it is still inside the SELECT
check_sleep_bucket("BEGIN;\nSELECT pwet_trace_cursor();\nROLLBACK;",
	2, 1, 'DECLARE and CLOSE inside a function');

# a WITH HOLD cursor is run to completion at COMMIT: its function's SQL
# belongs to the COMMIT
check_sleep_bucket(
	"BEGIN;\nDECLARE pwet_trace_hc CURSOR WITH HOLD FOR "
	  . "SELECT pwet_trace_sleepy(i) FROM generate_series(1, 2) i;\n"
	  . "COMMIT;",
	4, 2, 'WITH HOLD cursor materialized at commit');
run_sql($psql7, 'CLOSE pwet_trace_hc;');

# FETCH from a cursor declared by an earlier statement
check_sleep_bucket(
	"BEGIN;\nDECLARE pwet_trace_fc CURSOR FOR "
	  . "SELECT pwet_trace_sleepy(i) FROM generate_series(1, 2) i;\n"
	  . "FETCH ALL FROM pwet_trace_fc;\nCOMMIT;",
	4, 2, 'FETCH from a cursor declared earlier');

# Every PgSleep wait after seq $m of ring $pn7 is in one bucket, opened by
# the first depth-0 ExecStart after $m (a named prepared statement).
sub check_sleep_in_exec_bucket
{
	my ($m, $name) = @_;
	my $es = $node->safe_psql(
		'postgres', qq(
		SELECT min(seq) FROM pg_get_wait_event_trace($pn7)
		WHERE seq > $m AND wait_event = 'ExecStart' AND depth = 0;
	));
	my ($buckets, $calls, $raw) = split /\|/,
	  $node->safe_psql(
		'postgres', qq(
		SELECT coalesce(string_agg(DISTINCT bucket, ','), '') || '|'
		       || coalesce(sum(calls), 0) || '|'
		       || (SELECT count(*) FROM pg_get_wait_event_trace($pn7)
		           WHERE seq > $m AND wait_event = 'PgSleep')
		FROM pg_wait_event_trace_by_statement($pn7)
		WHERE wait_event = 'PgSleep'
		  AND (statement_seq > $m OR statement_seq IS NULL);
	));
	is($buckets, $es, "$name: the sleeps are in the bucket of the Execute")
	  or diag(markers_after($pn7, $m, 1), "\n", raw_sleeps($pn7, $m));
	cmp_ok($calls, '>=', 1, "$name: at least one PgSleep wait")
	  or diag(raw_sleeps($pn7, $m));
	is($calls, $raw, "$name: all of them");
}

$node->safe_psql(
	'postgres', q(
CREATE TABLE pwet_trace_pt (a int) PARTITION BY LIST (a);
CREATE TABLE pwet_trace_pt1 PARTITION OF pwet_trace_pt FOR VALUES IN (1);
CREATE TABLE pwet_trace_pt2 PARTITION OF pwet_trace_pt FOR VALUES IN (2);
CREATE FUNCTION pwet_trace_stable() RETURNS int STABLE LANGUAGE plpgsql
AS $body$
BEGIN
    PERFORM pg_sleep(0.1);
    RETURN 1;
END
$body$;
));
run_sql($psql7, 'SET plan_cache_mode = force_generic_plan;');
run_sql($psql7,
	'SELECT count(*) FROM pwet_trace_pt WHERE a = pwet_trace_stable() '
	  . '\parse pwet_trace_prune');
# first execution: the generic plan is made at Bind, and the planner
# evaluates the stable function for its estimate; then initial pruning
# evaluates it at executor start
$mark = idle_mark($pn7);
run_sql($psql7, '\bind_named pwet_trace_prune \g');
wait_for_marker($pn7, 'Idle', $mark);
check_sleep_in_exec_bucket($mark, 'generic plan made at Bind');
# second execution: the cached plan, initial pruning only
$mark = idle_mark($pn7);
run_sql($psql7, '\bind_named pwet_trace_prune \g');
wait_for_marker($pn7, 'Idle', $mark);
check_sleep_in_exec_bucket($mark, 'initial pruning at executor start');

# The depth of the last ExecStart of ring $pn after seq $m.
sub last_exec_depth
{
	my ($pn, $m) = @_;
	return $node->safe_psql(
		'postgres', qq(
		SELECT depth FROM pg_get_wait_event_trace($pn)
		WHERE seq > $m AND wait_event = 'ExecStart'
		ORDER BY seq DESC LIMIT 1;
	));
}

# A SHOW run by a volatile function that a stable function calls while the
# plan is made (an estimate) and started (initial pruning): its UtilityEnd
# is nested and does not end the statement, so one bucket holds every sleep.
$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_trace_vshow() RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    EXECUTE 'SHOW work_mem';
    PERFORM pg_sleep(0.1);
    RETURN 1;
END
$body$;
CREATE FUNCTION pwet_trace_est() RETURNS int STABLE LANGUAGE plpgsql
AS $body$
BEGIN
    RETURN (SELECT pwet_trace_vshow());
END
$body$;
));
check_sleep_bucket(
	'SELECT count(*) FROM pwet_trace_pt WHERE a = pwet_trace_est();',
	1, 2, 'SHOW run while the plan is made and started');

# An ExecutorStart that fails inside initial pruning, its error caught by
# an EXCEPTION block (a subtransaction): the statement after it is at
# depth 0 again.
$node->safe_psql(
	'postgres', q(
CREATE ROLE pwet_trace_r;
CREATE TABLE pwet_trace_denied (a int);
GRANT SELECT ON pwet_trace_pt TO pwet_trace_r;
CREATE FUNCTION pwet_trace_prune_catch() RETURNS int STABLE LANGUAGE plpgsql
AS $body$
BEGIN
    BEGIN
        PERFORM 1 FROM pwet_trace_denied;
    EXCEPTION WHEN insufficient_privilege THEN NULL;
    END;
    RETURN 1;
END
$body$;
));
run_sql($psql7, 'SET ROLE pwet_trace_r;');
run_sql($psql7,
	'SELECT count(*) FROM pwet_trace_pt WHERE a = pwet_trace_prune_catch() '
	  . '\parse pwet_trace_prune_catch');
run_sql($psql7, "\\bind_named pwet_trace_prune_catch \\g\n" x 2);
$mark = idle_mark($pn7);
run_sql($psql7, 'SELECT 1;');
wait_for_marker($pn7, 'Idle', $mark);
is(last_exec_depth($pn7, $mark), '0',
	'a caught error in ExecutorStart (initial pruning) leaves depth 0');
run_sql($psql7, 'RESET ROLE;');
$psql7->quit;

# Errors in ExecutorStart in a session that does not trace when the error
# happens, and inside savepoints and nested EXCEPTION blocks.
my $psql9 = $node->background_psql('postgres', on_error_stop => 0);
my ($pid9, $pn9) = ids_of($psql9);

# capture off: ExecutorStart fails (read-only transaction), the transaction
# aborts, then trace is switched on
run_sql($psql9, 'BEGIN READ ONLY;');
like(run_sql($psql9, 'INSERT INTO pwet_trace_denied VALUES (1);'),
	qr/read-only transaction/, 'INSERT fails in ExecutorStart');
run_sql($psql9, 'ROLLBACK;');
run_sql($psql9, 'SET pg_wait_event_tracing.capture = trace;');
$mark = idle_mark($pn9);
run_sql($psql9, 'SELECT 1;');
wait_for_marker($pn9, 'Idle', $mark);
is(last_exec_depth($pn9, $mark), '0',
	'an ExecutorStart that failed with capture off leaves depth 0');

# an ExecutorStart error inside savepoints, then ROLLBACK TO SAVEPOINT
run_sql($psql9, 'SET ROLE pwet_trace_r;');
$mark = idle_mark($pn9);
like(
	run_sql($psql9,
		"BEGIN;\nSAVEPOINT s1;\nSAVEPOINT s2;\n"
		  . "SELECT 1 FROM pwet_trace_denied;\nROLLBACK TO s2;\n"
		  . "SELECT 2;\nRELEASE s1;\nCOMMIT;"),
	qr/permission denied/,
	'the SELECT inside the savepoints fails in ExecutorStart');
wait_for_marker($pn9, 'Idle', $mark);
is(last_exec_depth($pn9, $mark), '0',
	'after ROLLBACK TO SAVEPOINT the next statement is at depth 0');

# nested EXCEPTION blocks inside a running query: after both catches, the
# function's next statement is at depth 1 (inside the outer SELECT's run)
$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_trace_nested_catch() RETURNS int LANGUAGE plpgsql
AS $body$
BEGIN
    BEGIN
        BEGIN
            PERFORM 1 FROM pwet_trace_denied;
        EXCEPTION WHEN insufficient_privilege THEN NULL;
        END;
        PERFORM 1 FROM pwet_trace_denied;
    EXCEPTION WHEN insufficient_privilege THEN NULL;
    END;
    PERFORM pg_sleep(0.1);
    RETURN 1;
END
$body$;
));
$mark = idle_mark($pn9);
run_sql($psql9, 'SELECT pwet_trace_nested_catch();');
wait_for_marker($pn9, 'Idle', $mark);
is( $node->safe_psql(
		'postgres', qq(
		SELECT depth FROM pg_get_wait_event_trace($pn9)
		WHERE seq > $mark AND wait_event = 'ExecStart'
		  AND seq < (SELECT min(seq) FROM pg_get_wait_event_trace($pn9)
		             WHERE seq > $mark AND wait_event = 'PgSleep')
		ORDER BY seq DESC LIMIT 1;
	)),
	'1',
	'after nested caught errors, the function\'s next statement is at depth 1')
  or diag(raw_sleeps($pn9, $mark));
$mark = idle_mark($pn9);
run_sql($psql9, 'SELECT 3;');
wait_for_marker($pn9, 'Idle', $mark);
is(last_exec_depth($pn9, $mark), '0', '...and the next query at depth 0');
run_sql($psql9, 'RESET ROLE;');
$psql9->quit;

# A fresh connection whose first executor call happens inside an open
# savepoint (nothing has run an executor in that backend before) and
# fails.  The session is found by application_name, so that no query of
# the test's own runs in it first.
my $psql10 = $node->background_psql(
	'postgres',
	on_error_stop => 0,
	connstr => $node->connstr('postgres') . ' application_name=pwet_fresh');
run_sql($psql10, 'SET pg_wait_event_tracing.capture = trace;');
like(
	run_sql($psql10,
		"BEGIN READ ONLY;
SAVEPOINT s;
"
		  . "INSERT INTO pwet_trace_denied VALUES (1);
ROLLBACK TO s;
"
		  . "SELECT pg_sleep(0.01);
COMMIT;
SELECT pg_sleep(0.01);"),
	qr/read-only transaction/,
	'fresh connection: the INSERT in the savepoint fails in ExecutorStart');
my $pn10 = $node->safe_psql(
	'postgres', q(
	SELECT id FROM pg_stat_get_backend_idset() AS id
	WHERE pg_stat_get_backend_pid(id) =
	      (SELECT pid FROM pg_stat_activity
	       WHERE application_name = 'pwet_fresh');
));
wait_for_marker($pn10, 'Idle', -1);
is( $node->safe_psql(
		'postgres', qq(
		SELECT count(*) FILTER (WHERE depth = 0) || ':'
		       || count(*) FILTER (WHERE depth <> 0)
		FROM pg_get_wait_event_trace($pn10)
		WHERE wait_event = 'ExecStart';
	)),
	'3:0',
	'fresh connection: every ExecStart, after the failure too, is at depth 0');
$psql10->quit;

# PL/Perl's return_next catches the error of a domain CHECK that fails in
# ExecutorStart (a SQL function reading a table the role may not read) and
# lets the function go on without a subtransaction rollback: the SPI query
# after it is still at depth 1, inside the outer SELECT.
SKIP:
{
	skip 'PL/Perl is not available', 2
	  unless $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_available_extensions WHERE name = 'plperl';"
	  ) eq '1';

	$node->safe_psql(
		'postgres', q(
CREATE EXTENSION plperl;
CREATE FUNCTION pwet_trace_chk(int) RETURNS bool STABLE LANGUAGE sql
    AS 'SELECT true FROM pwet_trace_denied LIMIT 1';
CREATE DOMAIN pwet_trace_dom AS int CHECK (pwet_trace_chk(VALUE));
CREATE FUNCTION pwet_trace_plperl() RETURNS SETOF pwet_trace_dom
LANGUAGE plperl AS $body$
    eval { return_next(1); };
    spi_exec_query('SELECT pg_sleep(0.1)');
    return;
$body$;
));
	my $psql11 = $node->background_psql('postgres');
	$psql11->query_safe('SET pg_wait_event_tracing.capture = trace;');
	my ($pid11, $pn11) = ids_of($psql11);
	run_sql($psql11, 'SET ROLE pwet_trace_r;');
	$mark = idle_mark($pn11);
	# core warns about resources the caught error left open; no ERROR
	unlike(run_sql($psql11, 'SELECT * FROM pwet_trace_plperl();'),
		qr/ERROR/, 'PL/Perl: the function caught the error and returned');
	wait_for_marker($pn11, 'Idle', $mark);
	is( $node->safe_psql(
			'postgres', qq(
			SELECT depth FROM pg_get_wait_event_trace($pn11)
			WHERE seq > $mark AND wait_event = 'ExecStart'
			  AND seq < (SELECT min(seq) FROM pg_get_wait_event_trace($pn11)
			             WHERE seq > $mark AND wait_event = 'PgSleep')
			ORDER BY seq DESC LIMIT 1;
		)),
		'1',
		'PL/Perl: the SPI query after the caught error is at depth 1')
	  or diag(raw_sleeps($pn11, $mark));
	$psql11->quit;
}

# A session that speaks the protocol itself (psql cannot send an Execute
# with a row limit), over $node->raw_connect() and with blocking send() and
# recv() on it, as core's own protocol tests do (src/test/postmaster/t/
# 004_negotiate.pl).  Only where raw_connect() works over a Unix-domain
# socket ($proto_ok below): the hand-made startup packet carries no
# password, and the test cluster trusts only those connections (on Windows
# TCP connections use SSPI).  The user is the one psql connects as, asked
# from the server (no getpwuid(), which Windows does not have).  Returns the
# backend pid and closures: send($type, $body), recv_until($type), which
# reads messages up to and including one of type $type and returns the
# types seen (and the BackendKeyData body, if any), and close().  The
# session dies if the server sends an error or closes the connection; a
# server that stops answering is bounded by the test's own timeout, as in
# core.
sub proto_connect
{
	my $user = $node->safe_psql('postgres', 'SELECT current_user;');
	my $sock = $node->raw_connect();

	my $send = sub {
		my ($type, $body) = @_;
		my $msg = $type . pack('N', length($body) + 4) . $body;
		while (length $msg)
		{
			my $n = $sock->send($msg);
			die "write to the server failed: $!" unless $n;
			substr($msg, 0, $n) = '';
		}
	};
	my $read_n = sub {
		my ($n) = @_;
		my $buf = '';
		while (length($buf) < $n)
		{
			# like core's tests, judge recv() by the data it returns (its
			# return value, the peer address, differs between platforms)
			my $got = '';
			$sock->recv($got, $n - length($buf));
			die "read from the server failed or the connection closed: $!"
			  if !defined($got) || $got eq '';
			$buf .= $got;
		}
		return $buf;
	};
	my $recv_until = sub {
		my ($until) = @_;
		my ($types, $key) = ('', undef);
		while (1)
		{
			my ($type, $len) = unpack('a N', $read_n->(5));
			my $body = $read_n->($len - 4);
			die "server error: " . join(' ', split /\0/, substr($body, 1))
			  if $type eq 'E';
			$key = $body if $type eq 'K';
			$types .= $type;
			return ($types, $key) if $type eq $until;
		}
	};

	$send->('', pack('N', 196608) . "user\0$user\0database\0postgres\0\0");
	my (undef, $key) = $recv_until->('Z');
	return {
		pid => unpack('N', $key),
		send => $send,
		recv_until => $recv_until,
		close => sub { $send->('X', ''); $sock->close(); },
	};
}

# raw_connect() over a Unix-domain socket, as proto_connect() needs.
my $proto_ok = $use_unix_sockets && $node->raw_connect_works();
my $proto_skip = 'needs raw_connect() over a Unix-domain socket';

# ---------------------------------------------------------------------
# (i) a suspended portal, resumed after a pause
# ---------------------------------------------------------------------
SKIP:
{
	skip $proto_skip, 6 unless $proto_ok;

	$node->safe_psql(
		'postgres', q(
CREATE FUNCTION pwet_trace_susp(i int) RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM pg_sleep(0.1);
    EXECUTE 'SHOW work_mem';
    RETURN i;
END
$body$;
));

	my $conn = proto_connect();
	my ($send, $recv_until, $pid8) =
	  ($conn->{send}, $conn->{recv_until}, $conn->{pid});
	$send->('Q', "SET pg_wait_event_tracing.capture = trace;\0");
	$recv_until->('Z');
	my $pn8 = $node->safe_psql('postgres',
		"SELECT id FROM pg_stat_get_backend_idset() AS id "
		  . "WHERE pg_stat_get_backend_pid(id) = $pid8;");
	wait_for_marker($pn8, 'Idle', -1);
	my $m8 = idle_mark($pn8);

	# Parse, Bind, Execute with a limit of one row, Flush
	$send->('P',
		"\0SELECT pwet_trace_susp(i) FROM generate_series(1, 2) i\0"
		  . pack('n', 0));
	$send->('B', "\0\0" . pack('n n n', 0, 0, 0));
	$send->('E', "\0" . pack('N', 1));
	$send->('H', '');
	my ($types) = $recv_until->('s');
	is($types, '12Ds', 'suspended portal: one row, then PortalSuspended');

	# the backend now waits for input in the middle of the statement
	$node->poll_query_until(
		'postgres', qq(
		SELECT wait_event = 'ClientRead' FROM pg_stat_activity
		WHERE pid = $pid8;
	)) or die "backend $pid8 did not wait for input";

	# resume, then Sync
	$send->('E', "\0" . pack('N', 0));
	$send->('S', '');
	($types) = $recv_until->('Z');
	is($types, 'DCZ', 'suspended portal: the second Execute returned the rest');
	wait_for_marker($pn8, 'Idle', $m8);
	$conn->{close}->();

	# pg_sleep() can complete more than one wait: the first PgSleep record
	# is the first Execute's, the last one the second Execute's.  With no
	# record at all (a stalled pg_sleep()) the bounds are -1, so that the
	# checks below fail with their diagnostics instead of stopping the test.
	my ($first, $second, $sleeps) = split /\|/,
	  $node->safe_psql(
		'postgres', qq(
		SELECT coalesce(min(seq), -1) || '|' || coalesce(max(seq), -1)
		       || '|' || count(*)
		FROM pg_get_wait_event_trace($pn8)
		WHERE seq > $m8 AND wait_event = 'PgSleep';
	));
	cmp_ok($sleeps, '>=', 2, 'suspended portal: a sleep in each Execute')
	  or diag(raw_sleeps($pn8, $m8));
	cmp_ok(
		$node->safe_psql(
			'postgres', qq(
			SELECT count(*) FROM pg_get_wait_event_trace($pn8)
			WHERE seq > $first AND seq < $second
			  AND wait_event = 'ClientRead';
		)),
		'>=', 1,
		'suspended portal: the backend waited for input between them')
	  or diag(raw_sleeps($pn8, $m8));
	is( $node->safe_psql(
			'postgres', qq(
			SELECT count(*) FROM pg_get_wait_event_trace($pn8)
			WHERE seq > $first AND seq < $second
			  AND wait_event IN ('Idle', 'IdleInTransaction');
		)),
		'0',
		'suspended portal: no idle marker inside the statement');
	is( $node->safe_psql(
			'postgres', qq(
			SELECT string_agg(DISTINCT bucket, ',') || ':' || sum(calls)
			FROM pg_wait_event_trace_by_statement($pn8)
			WHERE wait_event = 'PgSleep' AND statement_seq > $m8;
		)),
		$node->safe_psql(
			'postgres', qq(
			SELECT min(seq) || ':$sleeps' FROM pg_get_wait_event_trace($pn8)
			WHERE seq > $m8 AND wait_event = 'QueryStart' AND depth = 0;
		)),
		'suspended portal: both sleeps are in the SELECT\'s bucket')
	  or diag(raw_sleeps($pn8, $m8));
}

# ---------------------------------------------------------------------
# (j) the extension in a schema that is not on the search path: the
# by-statement function reaches its own objects through @extschema@
# ---------------------------------------------------------------------
$node->safe_psql('postgres', 'CREATE DATABASE pwet_schema_db;');
$node->safe_psql('pwet_schema_db',
	'CREATE SCHEMA pwet_ext; '
	  . 'CREATE EXTENSION pg_wait_event_tracing SCHEMA pwet_ext;');
is( $node->safe_psql(
		'pwet_schema_db', qq(
		SET search_path = '';
		SELECT count(*) > 0
		FROM pwet_ext.pg_wait_event_trace_by_statement($pn7);
	)),
	't',
	'by_statement() works with its schema off the search path');

# ---------------------------------------------------------------------
# (k) capture switched in the middle of things
# ---------------------------------------------------------------------

# A session whose capture is still off, and its procnumber.  Its first
# switch to trace attaches a new ring, which starts at seq 0, so its
# markers are those after seq -1.
sub off_session
{
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	my ($pid, $pn) = ids_of($s);
	return ($s, $pn);
}

# The depths of the markers of ring $pn named in @only, in order.
sub depths_of
{
	my ($pn, @only) = @_;
	return markers_after($pn, -1, 1, @only);
}

$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_trace_on() RETURNS void SECURITY DEFINER
LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
END
$body$;
CREATE FUNCTION pwet_trace_toggle_on() RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
    PERFORM pg_sleep(0.01);
    EXECUTE 'SHOW work_mem';
    RETURN 1;
END
$body$;
CREATE PROCEDURE pwet_trace_toggle_call() LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM 1;
    COMMIT;
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
    PERFORM pg_sleep(0.01);
    COMMIT;
    PERFORM pwet_trace_sleepy(1);
END
$body$;
CREATE FUNCTION pwet_trace_offon() RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM pg_sleep(0.01);
    PERFORM set_config('pg_wait_event_tracing.capture', 'off', false);
    PERFORM pg_sleep(0.01);
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
    PERFORM pg_sleep(0.01);
    EXECUTE 'SHOW work_mem';
    RETURN 1;
END
$body$;
CREATE FUNCTION pwet_trace_catch_only() RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    BEGIN
        PERFORM 1 / d FROM pwet_trace_divzero_rows;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
    RETURN 1;
END
$body$;
CREATE FUNCTION pwet_trace_catch_then_on() RETURNS int LANGUAGE plpgsql
AS $body$
BEGIN
    BEGIN
        PERFORM 1 / d FROM pwet_trace_divzero_rows;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
    PERFORM pg_sleep(0.01);
    RETURN 1;
END
$body$;
));

# Switched on inside a function called by a top-level SELECT: everything
# the function runs after the switch is nested (depth 1), the SELECT's
# ExecEnd is at depth 0, and so is the next statement.
my ($sk, $pnk) = off_session();
is(run_sql($sk, "SELECT pwet_trace_toggle_on();\nSELECT 2;"),
	'', 'switch inside a function: ran');
wait_for_marker($pnk, 'Idle', -1);
my $mk = depths_of($pnk);
like($mk, qr/^(?:[A-Za-z]+:[1-9][0-9]*,)+ExecEnd:0,/,
	'switch inside a function: every marker before its ExecEnd is nested')
  or diag($mk);
like($mk, qr/ExecStart:1,.*UtilityStart:1,UtilityEnd:1,.*ExecEnd:0,/,
	'switch inside a function: its statements are at depth 1');
like($mk, qr/ExecEnd:0,.*QueryStart:0,ExecStart:0,ExecEnd:0,/,
	'switch inside a function: the next statement is at depth 0');
$sk->quit;

# Switched on inside a CALL, between two COMMITs: the second COMMIT is
# inside the utility statement (depth 1), the CALL's own end is at depth 0.
# A statement the procedure runs is at executor depth 0 (no planner or
# executor call around it); the function it calls runs its own at depth 1.
($sk, $pnk) = off_session();
is(run_sql($sk, 'CALL pwet_trace_toggle_call();'), '',
	'switch inside a CALL: ran');
wait_for_marker($pnk, 'Idle', -1);
is(depths_of($pnk, @txn), 'TxnCommit:1,TxnCommit:0',
	'switch inside a CALL: the COMMIT inside is at depth 1, the end at 0');
is(depths_of($pnk, 'ExecStart'), 'ExecStart:0,ExecStart:0,ExecStart:1',
	'switch inside a CALL: executor depths of the statements after it');
$sk->quit;

# Switched on in a transaction block after a statement failed in
# ExecutorStart inside a savepoint (capture still off then).
($sk, $pnk) = off_session();
like(
	run_sql($sk,
		"BEGIN READ ONLY;\nSAVEPOINT s;\n"
		  . "INSERT INTO pwet_trace_denied VALUES (1);\nROLLBACK TO s;\n"
		  . "SET pg_wait_event_tracing.capture = trace;\n"
		  . "SELECT pg_sleep(0.01);\nCOMMIT;\nSELECT pg_sleep(0.01);"),
	qr/read-only transaction/,
	'switch after a failed statement in a savepoint: the INSERT failed');
wait_for_marker($pnk, 'Idle', -1);
is(depths_of($pnk, 'ExecStart'), 'ExecStart:0,ExecStart:0',
	'switch after a failed statement in a savepoint: both SELECTs at depth 0');
$sk->quit;

# Switched on after a WITH HOLD cursor was declared: its function's SQL,
# run at COMMIT, is nested and belongs to the COMMIT.
($sk, $pnk) = off_session();
is( run_sql(
		$sk,
		"BEGIN;\nDECLARE pwet_trace_hc2 CURSOR WITH HOLD FOR "
		  . "SELECT pwet_trace_sleepy(i) FROM generate_series(1, 2) i;\n"
		  . "SET pg_wait_event_tracing.capture = trace;\nCOMMIT;"),
	'',
	'switch with a WITH HOLD cursor open: ran');
wait_for_marker($pnk, 'Idle', -1);
is(depths_of($pnk, 'ExecStart'), 'ExecStart:1,ExecStart:1',
	'switch with a WITH HOLD cursor open: its SQL is nested');
my $hold_want = $node->safe_psql(
	'postgres', qq(
	SELECT min(seq) || ':' || (SELECT count(*)
	                          FROM pg_get_wait_event_trace($pnk)
	                          WHERE wait_event = 'PgSleep')
	FROM pg_get_wait_event_trace($pnk)
	WHERE wait_event = 'QueryStart' AND depth = 0;
));
like($hold_want, qr/^[0-9]+:([2-9]|[0-9]{2,})$/,
	'switch with a WITH HOLD cursor open: COMMIT opened, at least 2 sleeps')
  or diag(raw_sleeps($pnk, -1));
is( $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(DISTINCT bucket, ',') || ':' || sum(calls)
		FROM pg_wait_event_trace_by_statement($pnk)
		WHERE wait_event = 'PgSleep';
	)),
	$hold_want,
	'switch with a WITH HOLD cursor open: its sleeps are in COMMIT\'s bucket')
  or diag(raw_sleeps($pnk, -1));
run_sql($sk, 'CLOSE pwet_trace_hc2;');
$sk->quit;

# Switched on inside a utility statement that then fails: that statement
# wrote no UtilityStart, so it gets no UtilityEnd on error either (it does
# on a normal return); the abort ends it, and the next statement has its
# own bucket.  The procedure commits after the switch, as the abort would
# otherwise undo the switch itself.  Its own statements run at executor
# depth 0, its COMMIT at utility depth 1.
$node->safe_psql(
	'postgres', q(
CREATE PROCEDURE pwet_trace_on_fail() LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM set_config('pg_wait_event_tracing.capture', 'trace', false);
    COMMIT;
    PERFORM 1;
    RAISE EXCEPTION 'pwet utility fails';
END
$body$;
));
($sk, $pnk) = off_session();
like(run_sql($sk, 'CALL pwet_trace_on_fail();'),
	qr/pwet utility fails/,
	'switch inside a failing utility statement: it failed');
wait_for_marker($pnk, 'Idle', -1);
my $mku = idle_mark($pnk);
is(run_sql($sk, 'SELECT pg_sleep(0.1);'), '',
	'switch inside a failing utility statement: next statement ran');
wait_for_marker($pnk, 'Idle', $mku);
is( depths_of($pnk),
	'ExecEnd:0,TxnCommit:1,QueryStart:0,ExecStart:0,ExecEnd:0,TxnAbort:0,'
	  . 'Idle:0,QueryStart:0,ExecStart:0,ExecEnd:0,TxnCommit:0,Idle:0',
	'switch inside a failing utility statement: no UtilityEnd, then TxnAbort');
my $fail_want = $node->safe_psql(
	'postgres', qq(
	SELECT min(seq) || ':' || (SELECT count(*)
	                          FROM pg_get_wait_event_trace($pnk)
	                          WHERE wait_event = 'PgSleep')
	FROM pg_get_wait_event_trace($pnk)
	WHERE wait_event = 'QueryStart' AND depth = 0 AND seq > $mku;
));
like($fail_want, qr/^[0-9]+:[1-9][0-9]*$/,
	'switch inside a failing utility statement: SELECT opened, it slept')
  or diag(raw_sleeps($pnk, $mku));
is( $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(DISTINCT bucket, ',') || ':' || sum(calls)
		FROM pg_wait_event_trace_by_statement($pnk)
		WHERE wait_event = 'PgSleep';
	)),
	$fail_want,
	'switch inside a failing utility statement: the SELECT\'s sleep is its own')
  or diag(raw_sleeps($pnk, $mku));
$sk->quit;

# Switched off and on again inside a function: the new ring's markers are
# nested until the SELECT ends.
($sk, $pnk) = off_session();
run_sql($sk, 'SET pg_wait_event_tracing.capture = trace;');
is(run_sql($sk, "SELECT pwet_trace_offon();\nSELECT 2;"),
	'', 'off and on inside a function: ran');
wait_for_marker($pnk, 'Idle', -1);
$mk = depths_of($pnk);
like($mk, qr/^(?:[A-Za-z]+:[1-9][0-9]*,)+ExecEnd:0,/,
	'off and on inside a function: every marker before its ExecEnd is nested')
  or diag($mk);
like($mk, qr/ExecStart:1,.*UtilityStart:1,UtilityEnd:1,.*ExecEnd:0,.*ExecStart:0,/,
	'off and on inside a function: nested at 1, the next statement at 0');
$sk->quit;

# An error caught with capture off, then capture on: in a separate
# statement, and inside the same function.
($sk, $pnk) = off_session();
is(run_sql($sk, 'SELECT pwet_trace_catch_only();'), '',
	'caught error, capture off: ran');
run_sql($sk, "SET pg_wait_event_tracing.capture = trace;\n"
	  . "SELECT pwet_trace_sleepy(1);");
wait_for_marker($pnk, 'Idle', -1);
is(depths_of($pnk, 'ExecStart'), 'ExecStart:0,ExecStart:1',
	'caught error with capture off, then on: depths 0 and 1');
$sk->quit;

($sk, $pnk) = off_session();
is(run_sql($sk, "SELECT pwet_trace_catch_then_on();\nSELECT pwet_trace_sleepy(1);"),
	'', 'caught error, then on inside the function: ran');
wait_for_marker($pnk, 'Idle', -1);
is(depths_of($pnk, 'ExecStart'), 'ExecStart:1,ExecStart:0,ExecStart:1',
	'caught error, then on inside the function: depths 1, then 0 and 1');
$sk->quit;

# PL/Perl's return_next() catches the error without a rollback while
# capture is off; the function then switches capture on.
SKIP:
{
	skip 'PL/Perl is not available', 2
	  unless $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_extension WHERE extname = 'plperl';") eq
	  '1';

	$node->safe_psql(
		'postgres', q(
CREATE FUNCTION pwet_trace_plperl_on() RETURNS SETOF pwet_trace_dom
LANGUAGE plperl AS $body$
    eval { return_next(1); };
    spi_exec_query('SELECT pwet_trace_on()');
    spi_exec_query('SELECT pg_sleep(0.01)');
    return;
$body$;
));
	($sk, $pnk) = off_session();
	run_sql($sk, 'SET ROLE pwet_trace_r;');
	unlike(
		run_sql($sk,
			"SELECT * FROM pwet_trace_plperl_on();\nSELECT pwet_trace_sleepy(1);"
		),
		qr/ERROR/,
		'PL/Perl, caught with capture off: no error');
	wait_for_marker($pnk, 'Idle', -1);
	is(depths_of($pnk, 'ExecStart'), 'ExecStart:1,ExecStart:0,ExecStart:1',
		'PL/Perl, caught with capture off, then on: depths 1, then 0 and 1');
	$sk->quit;
}

# PL/Python catches a COMMIT that failed in a deferred trigger's nested
# query while capture was off, then switches capture on.
SKIP:
{
	skip 'PL/Python is not available', 2
	  unless $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_available_extensions WHERE name = 'plpython3u';"
	  ) eq '1';

	$node->safe_psql(
		'postgres', q(
CREATE EXTENSION plpython3u;
CREATE TABLE pwet_trace_dc (a int);
CREATE FUNCTION pwet_trace_dc_fail() RETURNS trigger LANGUAGE plpgsql
AS $body$
BEGIN
    PERFORM 1 / d FROM pwet_trace_divzero_rows;
    RETURN NULL;
END
$body$;
CREATE CONSTRAINT TRIGGER pwet_trace_dc_t AFTER INSERT ON pwet_trace_dc
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION pwet_trace_dc_fail();
CREATE PROCEDURE pwet_trace_py() LANGUAGE plpython3u AS $body$
plpy.execute("INSERT INTO pwet_trace_dc VALUES (1)")
try:
    plpy.commit()
except plpy.SPIError:
    pass
plpy.execute("SELECT set_config('pg_wait_event_tracing.capture', 'trace', false)")
plpy.execute("SELECT pg_sleep(0.01)")
plpy.execute("SELECT pwet_trace_sleepy(1)")
$body$;
));
	($sk, $pnk) = off_session();
	is(run_sql($sk, "CALL pwet_trace_py();\nSELECT pwet_trace_sleepy(1);"),
		'', 'PL/Python, failed commit caught with capture off: no error');
	wait_for_marker($pnk, 'Idle', -1);
	is( depths_of($pnk, 'ExecStart'),
		'ExecStart:0,ExecStart:0,ExecStart:1,ExecStart:0,ExecStart:1',
		'PL/Python, failed commit caught with capture off, then on: depths');
	$sk->quit;
}

# Switched on between two Executes of a suspended portal, from another
# portal of the same transaction: the resumed execution's SQL is nested.
SKIP:
{
	skip $proto_skip, 3 unless $proto_ok;

	my $conn = proto_connect();
	my ($send, $recv_until) = ($conn->{send}, $conn->{recv_until});
	my $pnp = $node->safe_psql('postgres',
		"SELECT id FROM pg_stat_get_backend_idset() AS id "
		  . "WHERE pg_stat_get_backend_pid(id) = $conn->{pid};");

	$send->('P',
		"\0SELECT pwet_trace_susp(i) FROM generate_series(1, 2) i\0"
		  . pack('n', 0));
	$send->('B', "p1\0\0" . pack('n n n', 0, 0, 0));
	$send->('E', "p1\0" . pack('N', 1));
	$send->('H', '');
	my ($types) = $recv_until->('s');
	is($types, '12Ds', 'switch with a suspended portal: first row');

	$send->('P', "s2\0SELECT pwet_trace_on()\0" . pack('n', 0));
	$send->('B', "p2\0s2\0" . pack('n n n', 0, 0, 0));
	$send->('E', "p2\0" . pack('N', 0));
	$send->('E', "p1\0" . pack('N', 0));
	$send->('S', '');
	($types) = $recv_until->('Z');
	is($types, '12DCDCZ',
		'switch with a suspended portal: capture on, then the rest');
	wait_for_marker($pnp, 'Idle', -1);
	$conn->{close}->();
	like(
		depths_of($pnp, 'QueryStart', 'ExecStart', 'UtilityStart',
			'UtilityEnd'),
		qr/^(?:[A-Za-z]+:1,)*ExecStart:1,(?:[A-Za-z]+:1,)*UtilityStart:1,UtilityEnd:1$/,
		'switch with a suspended portal: the resumed execution\'s SQL is nested'
	);
}

# ---------------------------------------------------------------------
# (l) the frames' callback adds nothing to any message: the same script
# gives the same output with and without the module
# ---------------------------------------------------------------------
my $plain = PostgreSQL::Test::Cluster->new('plain');
$plain->init;
$plain->append_conf('postgresql.conf', "debug_parallel_query = off\n");
$plain->start;

my $ctx_setup = q(
CREATE TABLE pwet_ctx_rows (d int);
INSERT INTO pwet_ctx_rows VALUES (1), (0);
CREATE FUNCTION pwet_ctx_inner() RETURNS text LANGUAGE plpgsql AS $body$
DECLARE
    c text;
BEGIN
    RAISE NOTICE 'inner notice';
    GET DIAGNOSTICS c = PG_CONTEXT;
    RETURN c;
END
$body$;
CREATE FUNCTION pwet_ctx_outer() RETURNS text LANGUAGE plpgsql AS $body$
DECLARE
    c text;
BEGIN
    SELECT pwet_ctx_inner() INTO c;
    RETURN c;
END
$body$;
CREATE FUNCTION pwet_ctx_err() RETURNS int LANGUAGE plpgsql AS $body$
BEGIN
    PERFORM 1 / d FROM pwet_ctx_rows;
    RETURN 1;
END
$body$;
CREATE PROCEDURE pwet_ctx_proc() LANGUAGE plpgsql AS $body$
BEGIN
    EXECUTE 'SELECT pwet_ctx_err()';
END
$body$;
);
my $ctx_script = q(
SELECT pwet_ctx_outer();
SELECT pwet_ctx_err();
CALL pwet_ctx_proc();
DO $body$
BEGIN
    PERFORM pwet_ctx_err();
EXCEPTION WHEN division_by_zero THEN
    RAISE NOTICE 'caught: %', SQLERRM;
END
$body$;
);
$node->safe_psql('postgres', $ctx_setup);
$plain->safe_psql('postgres', $ctx_setup);
# The capture setting is part of the script on both servers, so that the
# line numbers psql prints are the same; on the server without the module
# it only sets a placeholder.
my (undef, $plain_out, $plain_err) = $plain->psql('postgres',
	"SET pg_wait_event_tracing.capture = trace;\n$ctx_script",
	on_error_stop => 0);
like($plain_out, qr/PL\/pgSQL function pwet_ctx_inner\(\) line \d+ at GET DIAGNOSTICS/,
	'silent callback: the reference output has a PG_CONTEXT stack');
like($plain_err, qr/CONTEXT:.*\n.*PL\/pgSQL function pwet_ctx_err\(\)/,
	'silent callback: the reference errors have CONTEXT lines');
foreach my $level ('off', 'stats', 'trace')
{
	my (undef, $out, $err) = $node->psql('postgres',
		"SET pg_wait_event_tracing.capture = $level;\n$ctx_script",
		on_error_stop => 0);
	is($out, $plain_out, "silent callback, capture $level: same output");
	is($err, $plain_err, "silent callback, capture $level: same messages");
}
$plain->stop;

# ---------------------------------------------------------------------
# (m) parallel query with capture = trace
#
# The worker functions raise their NOTICE or ERROR only in a process other
# than the leader (the leader's pid is passed in), and the leader does not
# run the plan itself (parallel_leader_participation = off), so a message
# in the output proves that a worker ran this very execution and that its
# message was relayed through the error context stack saved in the
# parallel context.  Without one (no worker could be started, or the plan
# was not parallel), the case is skipped, not passed.
# ---------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE TABLE pwet_trace_par (a int);
INSERT INTO pwet_trace_par SELECT generate_series(1, 20000);
ANALYZE pwet_trace_par;
CREATE FUNCTION pwet_trace_wnotice(i int, leader int) RETURNS int
LANGUAGE plpgsql PARALLEL SAFE AS $body$
BEGIN
    IF pg_backend_pid() <> leader AND i % 5000 = 0 THEN
        RAISE NOTICE 'pwet worker notice %', i;
    END IF;
    RETURN i;
END
$body$;
CREATE FUNCTION pwet_trace_werr(i int, leader int) RETURNS int
LANGUAGE plpgsql PARALLEL SAFE AS $body$
BEGIN
    IF pg_backend_pid() <> leader AND i % 1000 = 0 THEN
        RAISE EXCEPTION 'pwet worker error %', i
            USING ERRCODE = 'division_by_zero';
    END IF;
    RETURN i;
END
$body$;
CREATE FUNCTION pwet_trace_par_catch(leader int) RETURNS int
LANGUAGE plpgsql AS $body$
BEGIN
    BEGIN
        PERFORM count(pwet_trace_werr(a, leader)) FROM pwet_trace_par;
    EXCEPTION WHEN division_by_zero THEN
        RAISE NOTICE 'pwet caught: %', SQLERRM;
    END;
    PERFORM pg_sleep(0.01);
    RETURN 1;
END
$body$;
));
my $par_settings =
	"SET parallel_setup_cost = 0;\nSET parallel_tuple_cost = 0;\n"
  . "SET min_parallel_table_scan_size = 0;\n"
  . "SET max_parallel_workers_per_gather = 2;\n"
  . "SET parallel_leader_participation = off;\n";
my $no_worker = 'no parallel worker ran this execution (none could be '
  . 'started, or the plan was not parallel)';
my $psqlm = $node->background_psql('postgres', on_error_stop => 0);
my ($pidm, $pnm) = ids_of($psqlm);
run_sql($psqlm, $par_settings . 'SET pg_wait_event_tracing.capture = trace;');

# The lines of $err that match none of the patterns in @allowed (a psql
# message line, or a CONTEXT line and its continuations).  A case may skip
# for want of workers only when its output is empty: any other message (a
# worker that failed to start, say) fails it.
sub unexpected_lines
{
	my ($err, @allowed) = @_;
	my @rest;
	foreach my $line (split /\r?\n/, $err)
	{
		push @rest, $line unless grep { $line =~ $_ } @allowed;
	}
	return join("\n", @rest);
}
my $ctx_worker = qr/^\s*parallel worker$/;

# Run $sql in session m; return its stderr and the markers it wrote (with
# depths, only those named in @only).
sub par_run
{
	my ($sql, @only) = @_;
	my $m = idle_mark($pnm);
	my $err = run_sql($psqlm, $sql);
	wait_for_marker($pnm, 'Idle', $m);
	return ($err, markers_after($pnm, $m, 1, @only));
}

# run to the end: the leader reads the workers' notices while it runs the
# plan and when it shuts the Gather down
my @notice_lines = (
	qr/^psql:<stdin>:[0-9]+: NOTICE:  pwet worker notice [0-9]+$/,
	qr/^CONTEXT:  PL\/pgSQL function pwet_trace_wnotice\(integer,integer\) line [0-9]+ at RAISE$/,
	$ctx_worker);
my ($err, $mk_par) =
  par_run("SELECT count(pwet_trace_wnotice(a, $pidm)) FROM pwet_trace_par;");
is(unexpected_lines($err, @notice_lines), '',
	'parallel, to the end: no output but worker notices');
SKIP:
{
	skip "parallel, to the end: $no_worker", 2
	  unless $err =~ /pwet worker notice/;
	unlike($err, qr/ERROR|WARNING/, 'parallel, to the end: worker notices, no error');
	is($mk_par, 'QueryStart:0,ExecStart:0,ExecEnd:0,TxnCommit:0,Idle:0',
		'parallel, to the end: one top-level statement');
}

# under LIMIT: the Gather is shut down within the same ExecutorRun
($err, $mk_par) = par_run(
	"SELECT pwet_trace_wnotice(a, $pidm) FROM pwet_trace_par "
	  . "WHERE a % 5000 = 0 LIMIT 2;");
is(unexpected_lines($err, @notice_lines), '',
	'parallel, under LIMIT: no output but worker notices');
SKIP:
{
	skip "parallel, under LIMIT: $no_worker", 2
	  unless $err =~ /pwet worker notice/;
	unlike($err, qr/ERROR|WARNING/, 'parallel, under LIMIT: worker notices, no error');
	is($mk_par, 'QueryStart:0,ExecStart:0,ExecEnd:0,TxnCommit:0,Idle:0',
		'parallel, under LIMIT: one top-level statement');
}

# the same query in a cursor fetched in pieces: a DECLARE gets no parallel
# plan, so no worker runs it; its markers are those of a cursor
($err, $mk_par) = par_run(
	"BEGIN;\nDECLARE pwet_trace_pc CURSOR FOR "
	  . "SELECT pwet_trace_wnotice(a, $pidm) FROM pwet_trace_par;\n"
	  . "FETCH 2 FROM pwet_trace_pc;\nFETCH 3 FROM pwet_trace_pc;\n"
	  . "CLOSE pwet_trace_pc;\nCOMMIT;",
	'ExecStart', 'ExecEnd', 'UtilityStart', 'UtilityEnd');
is($err, '', 'cursor over the parallel query: no worker message, no error');
is( $mk_par,
	'UtilityStart:0,UtilityEnd:0,UtilityStart:0,ExecStart:0,UtilityEnd:0,'
	  . 'UtilityStart:0,UtilityEnd:0,UtilityStart:0,UtilityEnd:0,'
	  . 'UtilityStart:0,ExecEnd:0,UtilityEnd:0,UtilityStart:0,UtilityEnd:0',
	'cursor over the parallel query: every marker at depth 0');

# a worker's error, caught by an EXCEPTION block around the parallel query:
# the function's next statement is nested, the next query top-level
($err, $mk_par) = par_run("SELECT pwet_trace_par_catch($pidm);\nSELECT 1;",
	'ExecStart');
# no ERROR may escape, also when the catch happened
is( unexpected_lines(
		$err,
		qr/^psql:<stdin>:[0-9]+: NOTICE:  pwet caught: pwet worker error [0-9]+$/,
		qr/^CONTEXT:  PL\/pgSQL function pwet_trace_par_catch\(integer\) line [0-9]+ at RAISE$/
	),
	'',
	'parallel, caught worker error: no output but the catch\'s notice');
SKIP:
{
	skip "parallel, caught worker error: $no_worker", 1
	  unless $err =~ /pwet caught: pwet worker error/;
	is($mk_par, 'ExecStart:0,ExecStart:1,ExecStart:1,ExecStart:0',
		'parallel, caught worker error: depths 0, 1, 1, then 0');
}

# a worker's error that ends the statement
($err, $mk_par) = par_run(
	"SELECT count(pwet_trace_werr(a, $pidm)) FROM pwet_trace_par;\nSELECT 1;",
	'ExecStart');
is( unexpected_lines(
		$err,
		qr/^psql:<stdin>:[0-9]+: ERROR:  pwet worker error [0-9]+$/,
		qr/^CONTEXT:  PL\/pgSQL function pwet_trace_werr\(integer,integer\) line [0-9]+ at RAISE$/,
		$ctx_worker),
	'',
	'parallel, worker error: no output but the worker\'s error');
SKIP:
{
	skip "parallel, worker error: $no_worker", 1
	  unless $err =~ /ERROR:  pwet worker error/;
	is($mk_par, 'ExecStart:0,ExecStart:0',
		'parallel, worker error: the next statement at depth 0');
}
$psqlm->quit;

# a portal whose plan has a Gather, fetched in pieces by Executes with a
# row limit: the executor runs it without workers then, so there is no
# worker notice (no 'N' message) at all
SKIP:
{
	skip $proto_skip, 2 unless $proto_ok;

	my $conn = proto_connect();
	my ($send, $recv_until) = ($conn->{send}, $conn->{recv_until});
	my $pnp = $node->safe_psql('postgres',
		"SELECT id FROM pg_stat_get_backend_idset() AS id "
		  . "WHERE pg_stat_get_backend_pid(id) = $conn->{pid};");
	$send->('Q', $par_settings . "SET pg_wait_event_tracing.capture = trace;\0");
	$recv_until->('Z');
	wait_for_marker($pnp, 'Idle', -1);
	my $mp = idle_mark($pnp);
	$send->('P',
		"\0SELECT pwet_trace_wnotice(a, $conn->{pid}) FROM pwet_trace_par\0"
		  . pack('n', 0));
	$send->('B', "\0\0" . pack('n n n', 0, 0, 0));
	$send->('E', "\0" . pack('N', 2));
	$send->('E', "\0" . pack('N', 2));
	$send->('E', "\0" . pack('N', 0));
	$send->('S', '');
	my ($types) = $recv_until->('Z');
	like($types, qr/^12DDsDDsD{19996}CZ$/,
		'parallel plan fetched in pieces: 2 rows, 2 rows, the rest, no worker message');
	wait_for_marker($pnp, 'Idle', $mp);
	$conn->{close}->();
	is(markers_after($pnp, $mp, 1, 'ExecStart', 'ExecEnd'),
		'ExecStart:0,ExecEnd:0',
		'parallel plan fetched in pieces: one top-level execution');
}

$node->stop;

done_testing();
