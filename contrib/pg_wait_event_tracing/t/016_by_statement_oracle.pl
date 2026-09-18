# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: pg_wait_event_trace_by_statement() against
# client-side ground truth.
#
# The client knows which statement it sent, and in which order.  This test
# runs a fixed script of statement shapes in one traced session, each
# statement carrying a wait it causes itself (pg_sleep() inside execution,
# or a relation lock held by a second session when the wait is meant to
# happen before the statement's start marker), and checks that each wait
# is in the bucket of the statement that caused it.
#
# The expected query_id of a statement is NOT taken from the trace.  Every
# statement calls ora_id(), which raises a NOTICE with the session's
# pg_stat_activity.query_id while the statement is running; the client
# collects it from the NOTICE.  Query jumbling ignores constants, so each
# statement gets a distinct structure (a different number of constant
# columns), and the collected ids are asserted to be pairwise distinct
# before they are used.  A statement is then found in the function's output
# by that id, and by the statement_seq order it has in the script.
#
# Waits are never counted exactly: Windows can record two PgSleep waits
# for one pg_sleep().  Where several waits are expected (a DO loop of 3
# sleeps) the test asserts "at least".
#
# Shapes:
#   simple          simple protocol, single statement
#   m1..m3          one simple-query message holding three statements
#   ext             extended protocol, unnamed statement (\bind)
#   np              named prepared statement, executed 3 times
#   sp              SQL PREPARE / EXECUTE
#   pl              prepared statement x3 in a pipeline with \syncpipeline
#   tx1, tx2        explicit transaction BEGIN; s1; s2; COMMIT
#   do              DO block looping 3 times over a cached statement
#   call            CALL of a procedure that sleeps, COMMITs, sleeps
#   err, after      a statement that sleeps and then fails, and the next one
#   lkb             lock wait during Bind of a named prepared statement
#   lks             lock wait during parse analysis of a simple query
#   (lock_timeout)  lock_timeout expiring during Bind: '<no-statement>'
#
#   (pipelined lock_timeout)  the same with SET and Bind in one flush:
#                   no Idle marker, the wait stays with the SET
#
# A second traced session C, with its own ring, covers transaction ends that
# are not statement ends.  fsync is on and C sets commit_delay, so that a
# commit usually waits (Timeout:CommitDelay, IO:WalWrite, IO:WalSync); a
# commit can skip some of these (another process may have flushed the WAL
# already), so these checks compare the function's output with the waits
# the ring actually holds instead of assuming any.  Such a run, and a run
# where the backend waits for input between two pipelined messages, still
# passes without exercising the rule it is meant for; the regression test's
# fixed marker streams (pg_wait_event_tracing_trace, cases 14-26) cover
# those rules deterministically.  Cases:
#   dsync           INSERT, Flush, pause (IdleInTransaction), then Sync:
#                   the commit's waits go to the INSERT
#   dm1..dm3        three INSERTs, Flush, pause, Sync: to the last one
#   dfail           INSERT, Flush, pause, then a Bind that hits
#                   lock_timeout: '<no-statement>', not the INSERT
#   txi             BEGIN; INSERT; pause; COMMIT: COMMIT keeps its waits
#   callrb          CALL of a procedure that ROLLBACKs inside: one bucket
#   callerr, after2 a CALL that fails, and the next statement
#   callrbe, after3 a CALL that ROLLBACKs and then fails, and the next one
#   f1, f1s         CALL that catches an execution error and COMMITs, then
#                   a SELECT Execute before the Sync
#   f1b, f1bs       the same without the COMMIT
#   f2, f2s         CALL that catches an error of a nested CALL and
#                   ROLLBACKs, then a SELECT Execute before the Sync
#   cs, css         CALL that COMMITs, then a SELECT Execute before the
#                   Sync: the waits after the COMMIT stay with the CALL
#   (VACUUM, CREATE INDEX CONCURRENTLY)  internal commits, one statement
#
# Not tested, because a test cannot produce them deterministically (they
# depend on whether the next client message is already buffered when the
# backend looks, or on the exact point at which a wait happens between
# two statements): the documented ambiguities of statements without a
# commit between them, of a buffered next request, and of pipelined Parse
# messages.

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
compute_query_id = on
fsync = on
));
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE TABLE ora_lock_t (id int);
INSERT INTO ora_lock_t VALUES (1);
CREATE TABLE ora_zero (d int);
INSERT INTO ora_zero VALUES (0);

-- Report the calling session's current top-level query_id.
CREATE FUNCTION ora_id(tag text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q bigint;
BEGIN
    SELECT query_id INTO q FROM pg_stat_activity WHERE pid = pg_backend_pid();
    RAISE NOTICE 'ORA:%:%', tag, q;
END $$;

CREATE PROCEDURE ora_proc() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('call');
    PERFORM pg_sleep(0.1);
    COMMIT;
    PERFORM pg_sleep(0.1);
END $$;

-- session C's objects
CREATE TABLE ora_w (c1 int, c2 int, c3 int, c4 int, c5 int, c6 int,
                    c7 int, c8 int, c9 int, c10 int, c11 int, c12 int);
CREATE TABLE ora_vac (a int);
INSERT INTO ora_vac SELECT generate_series(1, 100);

CREATE PROCEDURE ora_proc_rb() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('callrb');
    PERFORM pg_sleep(0.1);
    ROLLBACK;
    PERFORM pg_sleep(0.1);
    PERFORM pg_sleep(0.1) FROM ora_zero;
END $$;

CREATE PROCEDURE ora_proc_err() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('callerr');
    PERFORM pg_sleep(0.1);
    PERFORM 1 / d FROM ora_zero;
END $$;

CREATE PROCEDURE ora_proc_rbe() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('callrbe');
    PERFORM pg_sleep(0.1);
    ROLLBACK;
    PERFORM pg_sleep(0.1);
    PERFORM 1 / d FROM ora_zero;
END $$;

-- an execution error (the divisor comes from a table) caught by an
-- EXCEPTION block: ExecutorEnd of the failed statement never runs
CREATE PROCEDURE ora_proc_f1() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('f1');
    BEGIN
        PERFORM 1 / d FROM ora_zero;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
    PERFORM pg_sleep(0.1);
    COMMIT;
    PERFORM pg_sleep(0.1);
END $$;

CREATE PROCEDURE ora_proc_f1b() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('f1b');
    BEGIN
        PERFORM 1 / d FROM ora_zero;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
    PERFORM pg_sleep(0.1);
END $$;

-- an error of a nested utility statement caught by an EXCEPTION block
CREATE PROCEDURE ora_proc_inner_err() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_sleep(0.1);
    PERFORM 1 / d FROM ora_zero;
END $$;

CREATE PROCEDURE ora_proc_f2() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('f2');
    BEGIN
        CALL ora_proc_inner_err();
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
    PERFORM pg_sleep(0.1);
    ROLLBACK;
    PERFORM pg_sleep(0.1);
END $$;

CREATE PROCEDURE ora_proc_cs() LANGUAGE plpgsql AS $$
BEGIN
    PERFORM ora_id('cs');
    PERFORM pg_sleep(0.1);
    COMMIT;
    PERFORM pg_sleep(0.1);
END $$;
));

my $psql_a = $node->background_psql('postgres');
my $psql_b = $node->background_psql('postgres', on_error_stop => 0);

$psql_b->query_safe('SET pg_wait_event_tracing.capture = trace;');
my $b_pid = $psql_b->query_safe('SELECT pg_backend_pid();');
my $b_pn = $psql_b->query_safe(
	'SELECT id FROM pg_stat_get_backend_idset() AS id '
	  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();');
chomp($b_pid);
chomp($b_pn);

# ---------------------------------------------------------------------
# Driving session B
# ---------------------------------------------------------------------

# tag => [ query_id, ... ] as reported by ora_id(), in order of arrival
my %ids;
# errors seen on stderr by the last s_finish()
my $b_errors = '';
my $run_cnt = 0;

# Send $sql to session $s and return a token to wait for.  Does not wait
# for completion.
sub s_start
{
	my ($s, $sql) = @_;
	my $token = 'ORACLE_DONE_' . ++$run_cnt;
	$s->{stderr} = '';
	$s->{stdin} .= "$sql\n\\warn $token\n\\echo $token\n";
	# hand the input to psql now: the caller may poll before s_finish()
	$s->{run}->pump_nb;
	return $token;
}

# Wait for completion, collect ora_id() notices.
sub s_finish
{
	my ($s, $token) = @_;
	# token plus its line end: psql writes the text and the newline in two
	# separate writes, and a bare-token match can leave the newline behind
	pump_until($s->{run}, $s->{timeout}, \$s->{stdout}, qr/$token\r?\n/);
	pump_until($s->{run}, $s->{timeout}, \$s->{stderr}, qr/$token\r?\n/);
	my $err = $s->{stderr};
	$s->{stdout} = '';
	$s->{stderr} = '';
	while ($err =~ /NOTICE:\s+ORA:(\w+):(-?\d+)/g)
	{
		push @{ $ids{$1} }, $2;
	}
	$err =~ s/^.*NOTICE:.*\r?\n//mg;
	$err =~ s/^.*$token.*\r?\n//mg;
	$b_errors = $err;
}

sub b_start
{
	return s_start($psql_b, @_);
}

sub b_finish
{
	s_finish($psql_b, @_);
}

sub b_run
{
	b_finish(b_start(@_));
}

sub wait_for_b_blocked
{
	$node->poll_query_until(
		'postgres', qq(
		SELECT EXISTS (SELECT 1 FROM pg_locks
		               WHERE NOT granted AND locktype = 'relation'
		                 AND relation = 'ora_lock_t'::regclass
		                 AND pid = $b_pid);
	)) or die "backend $b_pid never blocked on the table lock";
}

# Wait until B has written an Idle marker after the transaction end of the
# statement it just finished: the newest *marker* of its ring (ordinary wait
# records are ignored, since a latch wakeup appends a ClientRead record after
# the Idle) is Idle, and newer than the newest TxnCommit.  The Idle marker is
# written by the begin hook of B's first ClientRead wait after a statement.  A
# request sent before this point can already be in the socket buffer when B
# finishes the previous one: B then never waits, writes no Idle, and what the
# request does lands in the bucket of the previous statement.
# pg_stat_activity.wait_event = 'ClientRead' would not do: it is set before
# the begin hook runs.
sub wait_for_b_idle_marker
{
	$node->poll_query_until(
		'postgres', qq(
		SELECT wait_event = 'Idle'
		       AND seq > coalesce((SELECT max(seq)
		                           FROM pg_get_wait_event_trace($b_pn)
		                           WHERE wait_event = 'TxnCommit'), -1)
		FROM pg_get_wait_event_trace($b_pn)
		WHERE wait_event_type = 'Query'
		ORDER BY seq DESC LIMIT 1;
	)) or die "backend $b_pid did not write its Idle marker";
}

# Run $sql in B while session A holds ACCESS EXCLUSIVE on ora_lock_t, and
# release it once B is known to wait (polling pg_locks; the only fixed
# delay is the hold that makes the wait long enough to be recorded).
sub b_run_blocked
{
	my ($sql) = @_;
	$psql_a->query_safe(
		"BEGIN;\nLOCK TABLE ora_lock_t IN ACCESS EXCLUSIVE MODE;");
	my $token = b_start($sql);
	wait_for_b_blocked();
	usleep(200_000);
	$psql_a->query_safe('COMMIT;');
	b_finish($token);
}

# A statement with $k constant columns, which is what makes its query_id
# differ from every other statement's.  $from is an optional FROM clause.
sub sel
{
	my ($k, $from) = @_;
	return 'SELECT pg_sleep(0.1)' . (', 1' x $k) . ($from ? " $from" : '');
}

sub sel_param
{
	my ($k, $from) = @_;
	return 'SELECT pg_sleep($1)' . (', 1' x $k) . ($from ? " $from" : '');
}

# The expected query_id of a plannable statement, from EXPLAIN (VERBOSE) in
# another session.  pg_stat_activity.query_id cannot serve here: it holds
# the id of the first statement of a message or transaction, so the second
# and third statement of a multi-statement string would report the first's.
my %id;

# For the diagnostics of a failed check: the raw PgSleep records of the ring
# for each tag's client call(s), taken when the tags are added to the script
# (the records since the previous tags were added; tags added together share
# the number).  A pg_sleep() that recorded no wait (its backend stalled for
# longer than the sleep between computing its deadline and testing it)
# shows as 0 raw records.
my (%raw_sleeps, %raw_from);

sub push_tags
{
	my ($list, $pn, @entries) = @_;
	push @$list, @entries;
	my $from = $raw_from{$pn} // -1;
	my ($now, $n) = split /\|/,
	  $node->safe_psql(
		'postgres', qq(
		SELECT coalesce(max(seq), -1) || '|'
		       || count(*) FILTER (WHERE wait_event = 'PgSleep'
		                           AND seq > $from)
		FROM pg_get_wait_event_trace($pn);
	));
	my $tags = join(',', map { $_->[0] } @entries);
	$raw_sleeps{ $_->[0] } =
	  "$n raw PgSleep records in ring $pn for the client call(s) of $tags"
	  foreach @entries;
	$raw_from{$pn} = $now;
}
sub explain_id
{
	my ($tag, $sql, $generic) = @_;
	my $out = $node->safe_psql('postgres',
		'EXPLAIN (VERBOSE, COSTS OFF' . ($generic ? ', GENERIC_PLAN' : '')
		  . ") $sql");
	$out =~ /Query Identifier: (-?\d+)/
	  or die "no Query Identifier in the plan of $tag: $out";
	$id{$tag} = $1;
}

# ---------------------------------------------------------------------
# The script.  [ tag, executions, minimum PgSleep calls over them ], in
# script order.  Statements whose id can be had from EXPLAIN get it before
# they run; utility statements (EXECUTE, DO, CALL) report their own id
# with ora_id().
# ---------------------------------------------------------------------
my @script;
my @from_notice = qw(sp do call);

# simple protocol, single statement
explain_id('simple', sel(0));
b_run(sel(0) . ';');
push_tags(\@script, $b_pn, [ 'simple', 1, 1 ]);

# one simple-query message, three statements (\; keeps them in one message)
explain_id('m1', sel(1));
explain_id('m2', sel(2));
explain_id('m3', sel(3));
b_run(sel(1) . " \\;\n" . sel(2) . " \\;\n" . sel(3) . ';');
push_tags(\@script, $b_pn, [ 'm1', 1, 1 ], [ 'm2', 1, 1 ], [ 'm3', 1, 1 ]);

# extended protocol, unnamed statement
explain_id('ext', sel_param(4), 1);
b_run(sel_param(4) . " \\bind 0.1 \\g");
push_tags(\@script, $b_pn, [ 'ext', 1, 1 ]);

# named prepared statement, three executions
explain_id('np', sel_param(5), 1);
b_run(sel_param(5) . " \\parse ora_np");
b_run("\\bind_named ora_np 0.1 \\g\n" x 3);
push_tags(\@script, $b_pn, [ 'np', 3, 3 ]);

# SQL PREPARE / EXECUTE: the bucket is the EXECUTE utility statement's
b_run('PREPARE ora_sp AS SELECT ora_id(\'sp\'), pg_sleep(0.1)'
	  . (', 1' x 6) . ';');
b_run('EXECUTE ora_sp;');
push_tags(\@script, $b_pn, [ 'sp', 1, 1 ]);

# pipeline with a sync between the executions: each execution commits,
# and the next one starts without an Idle in between
explain_id('pl', sel_param(7), 1);
b_run(sel_param(7) . " \\parse ora_pl");
b_run(  "\\startpipeline\n"
	  . "\\bind_named ora_pl 0.1 \\sendpipeline\n"
	  . "\\syncpipeline\n"
	  . "\\bind_named ora_pl 0.1 \\sendpipeline\n"
	  . "\\syncpipeline\n"
	  . "\\bind_named ora_pl 0.1 \\sendpipeline\n"
	  . "\\endpipeline");
push_tags(\@script, $b_pn, [ 'pl', 3, 3 ]);

# explicit transaction
explain_id('tx1', sel(8));
explain_id('tx2', sel(9));
b_run("BEGIN;\n" . sel(8) . ";\n" . sel(9) . ";\nCOMMIT;");
push_tags(\@script, $b_pn, [ 'tx1', 1, 1 ], [ 'tx2', 1, 1 ]);

# DO block, looping over a cached statement
b_run(  'DO $$ BEGIN PERFORM ora_id(\'do\'); '
	  . 'FOR i IN 1..3 LOOP PERFORM pg_sleep(0.1); END LOOP; END $$;');
push_tags(\@script, $b_pn, [ 'do', 1, 3 ]);

# CALL of a procedure that COMMITs inside, with a sleep on each side
b_run('CALL ora_proc();');
push_tags(\@script, $b_pn, [ 'call', 1, 2 ]);

# a statement that sleeps and then fails (the divisor comes from a table,
# so the error is raised during execution, after the sleep), then a normal
# statement in the same session
my $err_sql = sel(10, ', 1 / d FROM ora_zero');
explain_id('err', $err_sql);
b_run("$err_sql;");
like($b_errors, qr/division by zero/, 'the failing statement failed');
explain_id('after', sel(11));
b_run(sel(11) . ';');
push_tags(\@script, $b_pn, [ 'err', 1, 1 ], [ 'after', 1, 1 ]);

# lock wait during Bind of a named prepared statement: the statement was
# parsed before the lock was taken, so the wait is in plan cache
# revalidation, before the ExecStart that opens the statement
explain_id('lkb', sel_param(12, 'FROM ora_lock_t'), 1);
b_run(sel_param(12, 'FROM ora_lock_t') . " \\parse ora_lk");
b_run_blocked("\\bind_named ora_lk 0.1 \\g");
push_tags(\@script, $b_pn, [ 'lkb', 1, 1 ]);

# lock wait during parse analysis of a simple query, before QueryStart
explain_id('lks', sel(13, 'FROM ora_lock_t'));
b_run_blocked(sel(13, 'FROM ora_lock_t') . ';');
push_tags(\@script, $b_pn, [ 'lks', 1, 1 ]);

# lock_timeout expiring during Bind: no statement ever starts
b_run("SELECT count(*), 1, 2, 3 FROM ora_lock_t \\parse ora_lt");
b_run("SET lock_timeout = '100ms';");
$psql_a->query_safe("BEGIN;\nLOCK TABLE ora_lock_t IN ACCESS EXCLUSIVE MODE;");
wait_for_b_idle_marker();
b_run("\\bind_named ora_lt \\g");
$psql_a->query_safe('COMMIT;');
like($b_errors, qr/lock timeout/, 'Bind with an expiring lock_timeout fails');
b_run("RESET lock_timeout;");

# The same failure with SET, its Sync and the Bind sent in one client flush
# (psql does not flush at \syncpipeline).  One flush is not one server read,
# so B may or may not wait for input, and write an Idle marker, between the
# SET's commit and the Bind; the assertions below accept both orderings.
# Without an Idle the documented rule leaves the Lock wait with the SET
# statement, the latest opener; with one it is a failed request.
wait_for_b_idle_marker();    # the RESET's Idle must not land after the mark
my $pipe_mark = $node->safe_psql('postgres',
	"SELECT coalesce(max(seq), -1) FROM pg_get_wait_event_trace($b_pn);");
$psql_a->query_safe("BEGIN;\nLOCK TABLE ora_lock_t IN ACCESS EXCLUSIVE MODE;");
b_run(  "\\startpipeline\n"
	  . "SET lock_timeout = '100ms' \\bind \\sendpipeline\n"
	  . "\\syncpipeline\n"
	  . "\\bind_named ora_lt \\sendpipeline\n"
	  . "\\endpipeline");
$psql_a->query_safe('COMMIT;');
like($b_errors, qr/lock timeout/,
	'pipelined Bind with an expiring lock_timeout fails');
b_run("RESET lock_timeout;");

# ---------------------------------------------------------------------
# Session C: transaction ends that are not statement ends
# ---------------------------------------------------------------------
my $psql_c = $node->background_psql('postgres', on_error_stop => 0);
$psql_c->query_safe('SET pg_wait_event_tracing.capture = trace;');
# A commit that writes WAL waits commit_delay before flushing it, unless the
# WAL is flushed already; commit_siblings = 0 drops the other condition.
$psql_c->query_safe('SET commit_delay = 10000;');
$psql_c->query_safe('SET commit_siblings = 0;');
my $c_pn = $psql_c->query_safe(
	'SELECT id FROM pg_stat_get_backend_idset() AS id '
	  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();');
my $c_pid = $psql_c->query_safe('SELECT pg_backend_pid();');
chomp($c_pn);
chomp($c_pid);

sub c_run
{
	s_finish($psql_c, s_start($psql_c, @_));
}

# The newest seq of ring $pn.
sub ring_mark
{
	my ($pn) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(max(seq), -1) FROM pg_get_wait_event_trace($pn);");
}

# The newest seq of ring $pn once its session has gone idle: wait until the
# newest marker is Idle or IdleInTransaction first.  s_finish() returns when
# psql has the result, before the backend has necessarily blocked in
# ClientRead and written the Idle marker of the statement just run; a mark
# taken earlier could have that Idle land after it.
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
# The marker is written by the begin hook of the backend's first ClientRead
# wait after the statement; see wait_for_b_idle_marker().
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

# An INSERT with $k columns, sleeping once.
sub ins
{
	my ($k) = @_;
	return
	    'INSERT INTO ora_w ('
	  . join(', ', map { "c$_" } 1 .. $k)
	  . ') SELECT '
	  . join(', ', ('1') x $k)
	  . ' FROM pg_sleep(0.1)';
}

my @c_script;
my @c_from_notice = qw(callrb callerr callrbe f1 f1b f2 cs);
my @c_delayed;    # tags whose commit was run by a delayed Sync
my @c_util;       # [ name, ring position before it ]
my $mark;

# INSERT, Flush, and the Sync only after the backend waited for input
# (\flushrequest queues the Flush message, \flush sends it)
explain_id('dsync', ins(2));
$mark = idle_mark($c_pn);
c_run("\\startpipeline\n"
	  . ins(2)
	  . " \\bind \\sendpipeline\n\\flushrequest\n\\flush");
wait_for_marker($c_pn, 'IdleInTransaction', $mark);
c_run("\\endpipeline");
wait_for_marker($c_pn, 'Idle', $mark);
push_tags(\@c_script, $c_pn, [ 'dsync', 1, 1 ]);
push @c_delayed, 'dsync';

# three INSERTs before one delayed Sync: the commit goes to the last one
explain_id('dm1', ins(3));
explain_id('dm2', ins(4));
explain_id('dm3', ins(5));
$mark = idle_mark($c_pn);
c_run("\\startpipeline\n"
	  . ins(3) . " \\bind \\sendpipeline\n"
	  . ins(4) . " \\bind \\sendpipeline\n"
	  . ins(5) . " \\bind \\sendpipeline\n"
	  . "\\flushrequest\n\\flush");
wait_for_marker($c_pn, 'IdleInTransaction', $mark);
c_run("\\endpipeline");
wait_for_marker($c_pn, 'Idle', $mark);
push_tags(\@c_script, $c_pn, [ 'dm1', 1, 1 ], [ 'dm2', 1, 1 ], [ 'dm3', 1, 1 ]);
push @c_delayed, 'dm3';

# INSERT, Flush, pause, then a Bind that fails on lock_timeout before the
# Sync: a failed request, whose waits are not the INSERT's
explain_id('dfail', ins(6));
c_run("SELECT count(*), 1, 2, 3, 4 FROM ora_lock_t \\parse ora_lt_c");
c_run("SET lock_timeout = '100ms';");
$psql_a->query_safe("BEGIN;\nLOCK TABLE ora_lock_t IN ACCESS EXCLUSIVE MODE;");
$mark = idle_mark($c_pn);
c_run("\\startpipeline\n"
	  . ins(6)
	  . " \\bind \\sendpipeline\n\\flushrequest\n\\flush");
wait_for_marker($c_pn, 'IdleInTransaction', $mark);
c_run("\\bind_named ora_lt_c \\sendpipeline\n\\endpipeline");
$psql_a->query_safe('COMMIT;');
like($b_errors, qr/lock timeout/,
	'dfail: the Bind after the delayed INSERT fails on lock_timeout');
c_run("RESET lock_timeout;");
wait_for_marker($c_pn, 'Idle', $mark);
push_tags(\@c_script, $c_pn, [ 'dfail', 1, 1 ]);

# explicit transaction with a pause before COMMIT: COMMIT is a statement and
# keeps the waits of its commit
explain_id('txi', ins(7));
c_run('BEGIN;');
$mark = idle_mark($c_pn);
c_run(ins(7) . ';');
wait_for_marker($c_pn, 'IdleInTransaction', $mark);
c_run('COMMIT;');
wait_for_marker($c_pn, 'Idle', $mark);
push_tags(\@c_script, $c_pn, [ 'txi', 1, 1 ]);

# CALL of a procedure that ROLLBACKs inside: still one statement
c_run('CALL ora_proc_rb();');
push_tags(\@c_script, $c_pn, [ 'callrb', 1, 3 ]);

# a CALL whose error escapes, and the next statement
c_run('CALL ora_proc_err();');
like($b_errors, qr/division by zero/, 'callerr: the CALL failed');
explain_id('after2', sel(30));
c_run(sel(30) . ';');
push_tags(\@c_script, $c_pn, [ 'callerr', 1, 1 ], [ 'after2', 1, 1 ]);

# the same after a ROLLBACK inside the CALL
c_run('CALL ora_proc_rbe();');
like($b_errors, qr/division by zero/, 'callrbe: the CALL failed');
explain_id('after3', sel(31));
c_run(sel(31) . ';');
push_tags(\@c_script, $c_pn, [ 'callrbe', 1, 2 ], [ 'after3', 1, 1 ]);

# A CALL, then a SELECT as a second Execute before the one Sync.  Both
# statements are prepared beforehand, so the pipeline holds only Bind and
# Execute messages and no Parse runs ahead of an Execute.
sub call_then_select
{
	my ($call_tag, $proc, $min_sleeps, $sel_tag, $k) = @_;
	explain_id($sel_tag, sel($k));
	c_run("CALL $proc() \\parse ora_${call_tag}_c");
	c_run(sel($k) . " \\parse ora_${call_tag}_s");
	c_run(  "\\startpipeline\n"
		  . "\\bind_named ora_${call_tag}_c \\sendpipeline\n"
		  . "\\bind_named ora_${call_tag}_s \\sendpipeline\n"
		  . "\\endpipeline");
	push_tags(\@c_script, $c_pn, [ $call_tag, 1, $min_sleeps ],
		[ $sel_tag, 1, 1 ]);
}

# a caught execution error, then COMMIT inside the CALL
call_then_select('f1', 'ora_proc_f1', 2, 'f1s', 32);
# a caught execution error, no transaction end inside the CALL
call_then_select('f1b', 'ora_proc_f1b', 1, 'f1bs', 33);
# a caught error of a nested CALL, then ROLLBACK inside the outer CALL
call_then_select('f2', 'ora_proc_f2', 3, 'f2s', 34);
# COMMIT inside the CALL: the waits after it are still the CALL's
call_then_select('cs', 'ora_proc_cs', 2, 'css', 35);

# utility statements that commit inside
foreach my $u ('VACUUM ora_vac',
	'CREATE INDEX CONCURRENTLY ora_vac_i ON ora_vac (a)')
{
	$mark = idle_mark($c_pn);
	c_run("$u;");
	is($b_errors, '', "$u: succeeded");
	wait_for_marker($c_pn, 'Idle', $mark);
	push @c_util, [ $u, $mark ];
}

# ---------------------------------------------------------------------
# Collect the expected ids and the function's output
# ---------------------------------------------------------------------
foreach my $tag (@from_notice, @c_from_notice)
{
	my @got = @{ $ids{$tag} || [] };
	is(scalar @got, 1, "$tag: reported its query_id once");
	$id{$tag} = $got[0];
}
my @tags = map { $_->[0] } @script;
is(scalar(keys %{ { map { $_ => 1 } values %id } }),
	scalar(@tags) + scalar(@c_script),
	'the statements\' query_ids are pairwise distinct');
ok(!grep({ !$_ } values %id), '...and none of them is zero');

# B quiet: all its waits flushed to the ring
$node->poll_query_until(
	'postgres', qq(
	SELECT wait_event = 'ClientRead' FROM pg_stat_activity WHERE pid = $b_pid;
)) or die "backend $b_pid did not return to ClientRead";

# The function's output for ring $pn.
sub by_statement
{
	my ($pn) = @_;
	my @r;
	foreach my $line (
		split /\n/,
		$node->safe_psql(
			'postgres', qq(
			SELECT bucket, statement_seq, query_id, phase, wait_event_type,
			       wait_event, calls
			FROM pg_wait_event_trace_by_statement($pn);
		)))
	{
		my @f = split /\|/, $line, -1;
		push @r,
		  {
			bucket => $f[0],
			seq => $f[1] eq '' ? undef : $f[1],
			qid => $f[2] eq '' ? undef : $f[2],
			phase => $f[3],
			type => $f[4],
			event => $f[5],
			calls => $f[6],
		  };
	}
	return @r;
}
my @rows = by_statement($b_pn);

# The pipelined case, from the ring, in the interval after $pipe_mark: the
# SET's QueryStart, the Lock:relation record, and whether an Idle marker lies
# between the SET's TxnCommit and that record.
my ($piped_set_seq, $piped_lock_seq, $piped_idle) = split /\|/,
  $node->safe_psql(
	'postgres', qq(
	WITH t AS (SELECT * FROM pg_get_wait_event_trace($b_pn) WHERE seq > $pipe_mark),
	qs AS (SELECT min(seq) AS s FROM t WHERE wait_event = 'QueryStart'),
	lk AS (SELECT min(seq) AS s FROM t
	       WHERE wait_event_type = 'Lock' AND wait_event = 'relation'),
	tc AS (SELECT max(t.seq) AS s FROM t, qs, lk
	       WHERE t.wait_event = 'TxnCommit' AND t.seq > qs.s AND t.seq < lk.s)
	SELECT qs.s || '|' || lk.s || '|'
	       || (SELECT count(*) > 0 FROM t
	           WHERE wait_event = 'Idle' AND seq > tc.s AND seq < lk.s)::int
	FROM qs, lk, tc;
));
defined $piped_idle
  or die "no SET QueryStart, TxnCommit and Lock:relation record after the pipeline: "
  . ring_tail();

# rows of statement $tag for $type:$event, in output @$rows
sub rows_in
{
	my ($rows, $tag, $type, $event) = @_;
	return grep {
		defined $_->{qid}
		  && $_->{qid} eq $id{$tag}
		  && $_->{type} eq $type
		  && $_->{event} eq $event
	} @$rows;
}

sub rows_of
{
	return rows_in(\@rows, @_);
}

# ---------------------------------------------------------------------
# Every statement's sleep is in its own bucket(s), in script order
# ---------------------------------------------------------------------
sub check_script
{
	my ($script, $rows) = @_;
	my $prev_max = -1;
	foreach my $s (@$script)
	{
		my ($tag, $execs, $min_calls) = @$s;
		my @r = rows_in($rows, $tag, 'Timeout', 'PgSleep');
		my %seqs = map { $_->{seq} => 1 } @r;
		my $sleeps = 0;
		$sleeps += $_->{calls} foreach @r;

		is(scalar keys %seqs, $execs,
			"$tag: PgSleep waits are in exactly $execs statement bucket(s)")
		  or diag($raw_sleeps{$tag});
		cmp_ok($sleeps, '>=', $min_calls,
			"$tag: at least $min_calls PgSleep call(s) in them")
		  or diag($raw_sleeps{$tag});
		is(join(',', sort { $a cmp $b } map { $_->{phase} } @r),
			join(',', ('statement') x scalar @r),
			"$tag: ...in phase 'statement'");

		my ($min, $max) = (sort { $a <=> $b } keys %seqs)[ 0, -1 ];
		cmp_ok($min // -1, '>', $prev_max,
			"$tag: its bucket(s) come after those of the previous statement")
		  or diag($raw_sleeps{$tag});
		$prev_max = $max // $prev_max;
	}

	# a PgSleep anywhere else, an inner statement's own bucket for example
	my %known = map { $_ => 1 } values %id;
	my @stray = grep {
		$_->{event} eq 'PgSleep' && !$known{ $_->{qid} // '' }
	} @$rows;
	is(scalar @stray, 0, 'every PgSleep wait is in a bucket of the script')
	  or diag(join(', ',
			map { "$_->{bucket}:" . ($_->{qid} // '') . "=$_->{calls}" }
			  @stray));
}
check_script(\@script, \@rows);

# Waits before the start marker: phase 'pre' of the statement's bucket
foreach my $tag (qw(lkb lks))
{
	my @r = rows_of($tag, 'Lock', 'relation');
	ok(@r == 1 && $r[0]{phase} eq 'pre' && $r[0]{calls} >= 1,
		"$tag: Lock:relation wait is in the statement's bucket, phase pre");
}

# The newest markers and Lock records of B's ring, for diagnostics.
sub ring_tail
{
	return $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(seq || ':' || wait_event_type || '/' || wait_event,
		                  ' ' ORDER BY seq)
		FROM (SELECT * FROM pg_get_wait_event_trace($b_pn)
		      WHERE wait_event_type IN ('Query', 'Lock')
		      ORDER BY seq DESC LIMIT 60) s;
	));
}

# lock_timeout during Bind: the wait is in <no-statement>, which has
# no statement_seq, query_id or phase.  By definition that bucket holds every
# non-Client wait between the Idle marker and the TxnAbort that follows with
# no statement start in between, so the Lock:relation wait that was
# provoked must be there, but waits of the kinds that can really occur on that
# path may be there too: an LWLock (the lock manager partition, contended with
# the other session releasing its lock at the same moment, or a buffer
# mapping), or an IO / Buffer wait (a catalog or relcache page read while the
# plan is revalidated or while the failed request is cleaned up).  Anything
# else, notably a PgSleep (Timeout), is an attribution error.
my @nostmt = grep { $_->{bucket} eq '<no-statement>' } @rows;
my @nostmt_lock = grep { $_->{type} eq 'Lock' && $_->{event} eq 'relation' } @nostmt;
my @nostmt_other = grep {
	!($_->{type} eq 'Lock' && $_->{event} eq 'relation')
	  && $_->{type} !~ /^(LWLock|IO|Buffer)$/
} @nostmt;
ok(@nostmt_lock == 1 && $nostmt_lock[0]{calls} >= 1 + $piped_idle
	  && !@nostmt_other,
	'lock_timeout during Bind: Lock:relation wait is in <no-statement>')
  or diag('<no-statement> rows: '
	  . join(', ', map { "$_->{type}:$_->{event}=$_->{calls}" } @nostmt)
	  . '; all Lock:relation rows: '
	  . join(', ',
		map { "[$_->{bucket} seq=" . ($_->{seq} // '') . " phase="
			  . ($_->{phase} // '') . " calls=$_->{calls}]" }
		grep { $_->{type} eq 'Lock' && $_->{event} eq 'relation' } @rows)
	  . '; ring markers and Lock records (newest 60): '
	  . ring_tail());

ok( !grep({ defined $_->{seq} || defined $_->{qid} || $_->{phase} ne '' }
		@nostmt),
	'...which carries no statement_seq, query_id or phase');

# The pipelined failure, in the interval after $pipe_mark.  $piped_idle was
# computed from the ring above; the function must agree with it:
# - no Idle between the SET's TxnCommit and the Lock:relation record: the
#   lock row is in the SET's own bucket (statement_seq of the SET's
#   QueryStart), phase 'statement';
# - an Idle there: the lock row is the second one in <no-statement>.
note($piped_idle
	? 'pipelined case: B wrote an Idle between the SET and the Bind'
	: 'pipelined case: the Bind was already buffered, no Idle');
my %script_qid = map { $id{$_} => 1 } @tags;
my @piped_stmt_lock = grep {
	$_->{type} eq 'Lock' && $_->{event} eq 'relation'
	  && defined $_->{seq} && !$script_qid{ $_->{qid} // '' }
} @rows;
if ($piped_idle)
{
	is(scalar @piped_stmt_lock, 0,
		'pipelined lock_timeout with an Idle: no statement bucket has the lock wait')
	  or diag(ring_tail());
	cmp_ok($nostmt_lock[0]{calls}, '>=', 2,
		'pipelined lock_timeout with an Idle: the lock wait is in <no-statement>')
	  or diag(ring_tail());
}
else
{
	ok( @piped_stmt_lock == 1
		  && $piped_stmt_lock[0]{seq} == $piped_set_seq
		  && $piped_stmt_lock[0]{phase} eq 'statement'
		  && $piped_stmt_lock[0]{calls} >= 1,
		'pipelined lock_timeout without Idle: the lock wait is in the SET\'s '
		  . "bucket (statement_seq $piped_set_seq), phase statement")
	  or diag(join(', ',
			map { "$_->{bucket} seq=$_->{seq} phase=$_->{phase} calls=$_->{calls}" }
			  @piped_stmt_lock)
		  . '; ' . ring_tail());
}

# ---------------------------------------------------------------------
# Final: nothing lost into the idle buckets, and the invariant
# ---------------------------------------------------------------------
my @lost = grep {
	($_->{bucket} eq '<idle>' || $_->{bucket} eq '<unattributed>')
	  && ($_->{event} eq 'PgSleep' || $_->{type} eq 'Lock')
} @rows;
is(scalar @lost, 0,
	'no PgSleep or Lock wait in <idle> or <unattributed>')
  or diag(join(', ',
		map { "$_->{bucket}:$_->{type}:$_->{event}=$_->{calls}" } @lost));

# Every wait record is counted exactly once.  An idle backend can still
# append a record between two reads of its ring (a ClientRead ended by a
# catchup interrupt, for example), so the ring is counted before and after
# the function, and the three reads are repeated until the two counts agree;
# then the sum must equal them.  The ring must not have wrapped (its first
# record is still seq 0), or records would leave it between the reads.
sub check_conservation
{
	my ($pn, $name) = @_;
	my $count_sql = qq(SELECT count(*) FROM pg_get_wait_event_trace($pn)
	                   WHERE wait_event_type <> 'Query';);
	my ($before, $sum, $after);
	foreach my $try (1 .. 50)
	{
		my %t = (timeout => $PostgreSQL::Test::Utils::timeout_default);
		$before = $node->safe_psql('postgres', $count_sql, %t);
		$sum = $node->safe_psql('postgres',
			"SELECT sum(calls) FROM pg_wait_event_trace_by_statement($pn);",
			%t);
		$after = $node->safe_psql('postgres', $count_sql, %t);
		last if $before == $after;
		usleep(100_000);
	}
	is($node->safe_psql('postgres',
			"SELECT min(seq) FROM pg_get_wait_event_trace($pn);"),
		'0', "$name: the ring has not wrapped");
	is($before, $after, "$name: the ring did not change while it was read");
	is($sum, $before, $name);
}
check_conservation($b_pn,
	'sum(calls) equals the number of non-marker records of the ring');

# ---------------------------------------------------------------------
# Session C
# ---------------------------------------------------------------------
$node->poll_query_until(
	'postgres', qq(
	SELECT wait_event = 'ClientRead' FROM pg_stat_activity WHERE pid = $c_pid;
)) or die "backend $c_pid did not return to ClientRead";

my @c_rows = by_statement($c_pn);
my @c_ring;
foreach my $line (
	split /\n/,
	$node->safe_psql(
		'postgres', qq(
		SELECT seq, wait_event_type, wait_event, depth
		FROM pg_get_wait_event_trace($c_pn) ORDER BY seq;
	)))
{
	my @f = split /\|/, $line, -1;
	push @c_ring,
	  { seq => $f[0], type => $f[1], event => $f[2], depth => $f[3] };
}

note('C: ' . join(' ', map { "[$_->{bucket} " . ($_->{qid} // '') . " "
		  . ($_->{phase} // '') . " $_->{type}:$_->{event}=$_->{calls}]" } @c_rows));
note('C: tags: ' . join(' ', map { "$_->[0]=$id{$_->[0]}" } @c_script));
check_script(\@c_script, \@c_rows);

# The first marker of C's ring after seq $after that ends a statement
# interval: Idle, IdleInTransaction, or TxnCommit/TxnAbort written outside
# any utility statement (depth 0).
sub next_closer
{
	my ($after) = @_;
	foreach my $r (@c_ring)
	{
		next if $r->{seq} <= $after || $r->{type} ne 'Query';
		return $r
		  if $r->{event} =~ /^(Idle|IdleInTransaction)$/
		  || ($r->{event} =~ /^Txn(Commit|Abort)$/ && $r->{depth} == 0);
	}
	return undef;
}

sub next_marker
{
	my ($after, $event) = @_;
	foreach my $r (@c_ring)
	{
		return $r
		  if $r->{seq} > $after
		  && $r->{type} eq 'Query'
		  && $r->{event} eq $event;
	}
	return undef;
}

# Wait records of C's ring strictly between seqs $lo and $hi, as
# { 'type:event' => count }; without Client waits if $no_client.
sub ring_counts
{
	my ($lo, $hi, $no_client) = @_;
	my %n;
	foreach my $r (@c_ring)
	{
		next if $r->{type} eq 'Query' || $r->{seq} <= $lo || $r->{seq} >= $hi;
		next if $no_client && $r->{type} eq 'Client';
		$n{"$r->{type}:$r->{event}"}++;
	}
	return \%n;
}

sub add_counts
{
	my %n;
	foreach my $c (@_)
	{
		$n{$_} += $c->{$_} foreach keys %$c;
	}
	return \%n;
}

sub fmt_counts
{
	my ($c) = @_;
	return join(', ', map { "$_=$c->{$_}" } sort keys %$c) || '(none)';
}

# Phase 'statement' rows of the bucket opened at seq $open.
sub bucket_counts
{
	my ($open) = @_;
	my %n;
	foreach my $r (@c_rows)
	{
		$n{"$r->{type}:$r->{event}"} += $r->{calls}
		  if defined $r->{seq}
		  && $r->{seq} == $open
		  && $r->{phase} eq 'statement';
	}
	return \%n;
}

# The markers of C's ring after seq $after, for diagnostics.
sub c_markers
{
	my ($after) = @_;
	return join(' ',
		map { "$_->{seq}:$_->{event}" . ($_->{depth} ? "($_->{depth})" : '') }
		grep { $_->{type} eq 'Query' && $_->{seq} > $after } @c_ring)
	  || '(none)';
}

# The statement_seq of the one bucket of statement $tag.
sub open_seq
{
	my ($tag) = @_;
	my @r = rows_in(\@c_rows, $tag, 'Timeout', 'PgSleep');
	return @r ? $r[0]{seq} : -1;
}

# CALL, then SELECT before the Sync.  The pipeline is one client flush, but
# not necessarily one server read: if the backend waited for input between
# the two, it wrote an idle marker there and the SELECT is top-level for that
# reason alone.  Say which case this run tested.
foreach my $pair ([ 'f1', 'f1s' ], [ 'f1b', 'f1bs' ], [ 'f2', 'f2s' ],
	[ 'cs', 'css' ])
{
	my ($call, $sel) = map { open_seq($_) } @$pair;
	my $idle = grep {
		$_->{type} eq 'Query'
		  && $_->{event} =~ /^Idle/
		  && $_->{seq} > $call
		  && $_->{seq} < $sel
	} @c_ring;
	note("$pair->[0]: "
		  . ($idle
			? 'the backend waited for input between the CALL and the SELECT'
			: 'the SELECT followed the CALL without a wait for input'));
}

# A commit run by a Sync that came after the backend had waited for input:
# the statement's bucket holds every wait from its opening marker to the
# IdleInTransaction, and every non-Client wait from there to the closer after
# the TxnCommit.  Whether the commit waited at all depends on the WAL already
# flushed by others, so the expected waits are taken from the ring.
foreach my $tag (@c_delayed)
{
	my $open = open_seq($tag);
	my $iit = next_closer($open);
	my $tc = $iit && next_closer($iit->{seq});
	my $end = $tc && next_closer($tc->{seq});
	ok( $iit
		  && $iit->{event} eq 'IdleInTransaction'
		  && $tc->{event} eq 'TxnCommit'
		  && $end,
		"$tag: IdleInTransaction, then the TxnCommit of the delayed Sync")
	  or diag(c_markers($open));
	next unless $end;
	my $commit = ring_counts($iit->{seq}, $end->{seq}, 1);
	note("$tag: non-Client waits of the delayed commit: "
		  . fmt_counts($commit));
	my $want = add_counts(ring_counts($open, $iit->{seq}, 0), $commit);
	is_deeply(bucket_counts($open), $want,
		"$tag: its bucket holds its own waits and those of the delayed commit")
	  or diag('bucket: ' . fmt_counts(bucket_counts($open))
		  . '; expected: ' . fmt_counts($want));
}

# A Bind failing after the pause: '<no-statement>' holds exactly the
# non-Client waits between the IdleInTransaction and the TxnAbort (it is the
# only failed request of session C), and the INSERT none of them.
{
	my $open = open_seq('dfail');
	my $iit = next_closer($open);
	my $ab = $iit && next_closer($iit->{seq});
	ok($iit && $iit->{event} eq 'IdleInTransaction' && $ab->{event} eq 'TxnAbort',
		'dfail: IdleInTransaction, then the TxnAbort of the failed Bind')
	  or diag(c_markers($open));
	if ($ab)
	{
		my $fail = ring_counts($iit->{seq}, $ab->{seq}, 1);
		my %nostmt;
		$nostmt{"$_->{type}:$_->{event}"} += $_->{calls}
		  foreach grep { $_->{bucket} eq '<no-statement>' } @c_rows;
		cmp_ok($fail->{'Lock:relation'} // 0, '>=', 1,
			'dfail: the failed Bind waited for the lock');
		is_deeply(\%nostmt, $fail,
			'dfail: <no-statement> holds the waits of the failed Bind')
		  or diag('<no-statement>: ' . fmt_counts(\%nostmt)
			  . '; ring: ' . fmt_counts($fail));
		is_deeply(bucket_counts($open), ring_counts($open, $iit->{seq}, 0),
			'dfail: the INSERT\'s bucket holds only its own waits');
	}
}

# BEGIN, INSERT, pause, COMMIT: the INSERT keeps its own waits, the COMMIT
# statement those of its commit and of the cleanup after it.
{
	my $open = open_seq('txi');
	my $iit = next_closer($open);
	my $commit_qs = $iit && next_marker($iit->{seq}, 'QueryStart');
	my $tc = $commit_qs && next_closer($commit_qs->{seq});
	my $end = $tc && next_closer($tc->{seq});
	ok( $iit
		  && $iit->{event} eq 'IdleInTransaction'
		  && $tc->{event} eq 'TxnCommit'
		  && $end,
		'txi: IdleInTransaction after the INSERT, then COMMIT and its TxnCommit')
	  or diag(c_markers($open));
	if ($end)
	{
		note('txi: waits of COMMIT: '
			  . fmt_counts(ring_counts($commit_qs->{seq}, $end->{seq}, 1)));
		is_deeply(bucket_counts($open), ring_counts($open, $iit->{seq}, 0),
			'txi: the INSERT\'s bucket holds only its own waits');
		is_deeply(
			bucket_counts($commit_qs->{seq}),
			ring_counts($commit_qs->{seq}, $end->{seq}, 0),
			'txi: COMMIT\'s bucket holds the waits of its commit');
	}
}

# Utility statements that commit inside: those commits carry a nonzero
# depth, and the statement is one bucket holding all its waits.
foreach my $u (@c_util)
{
	my ($name, $lo) = @$u;
	my $qs = next_marker($lo, 'QueryStart');
	my $tc = $qs && next_closer($qs->{seq});
	my $end = $tc && next_closer($tc->{seq});
	ok($tc && $tc->{event} eq 'TxnCommit' && $end,
		"$name: ends with a depth-0 TxnCommit")
	  or diag(c_markers($lo));
	next unless $end;
	my @inner = grep {
		$_->{type} eq 'Query'
		  && $_->{event} eq 'TxnCommit'
		  && $_->{depth} > 0
		  && $_->{seq} > $qs->{seq}
		  && $_->{seq} < $tc->{seq}
	} @c_ring;
	ok(@inner >= 1, "$name: commits inside it carry a nonzero depth")
	  or diag(c_markers($lo));
	my @other = grep {
		defined $_->{seq} && $_->{seq} > $qs->{seq} && $_->{seq} < $end->{seq}
	} @c_rows;
	is(scalar @other, 0, "$name: no other bucket opens inside it");
	is_deeply(bucket_counts($qs->{seq}),
		ring_counts($qs->{seq}, $end->{seq}, 0),
		"$name: its bucket holds all its waits");
}

my @c_lost = grep {
	($_->{bucket} eq '<idle>' || $_->{bucket} eq '<unattributed>')
	  && ($_->{event} eq 'PgSleep' || $_->{type} eq 'Lock'
		|| $_->{event} eq 'CommitDelay')
} @c_rows;
is(scalar @c_lost, 0,
	'C: no PgSleep, Lock or CommitDelay wait in <idle> or <unattributed>')
  or diag(join(', ',
		map { "$_->{bucket}:$_->{type}:$_->{event}=$_->{calls}" } @c_lost));

check_conservation($c_pn,
	'C: sum(calls) equals the number of non-marker records of the ring');

$psql_a->quit;
$psql_b->quit;
$psql_c->quit;
$node->stop;
done_testing();
