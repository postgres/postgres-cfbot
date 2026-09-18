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
# Not tested, because a test cannot produce them deterministically (they
# depend on whether the next client message is already buffered when the
# backend looks, or on the exact point at which a wait happens between
# two statements): the documented ambiguities of statements without a
# commit between them, of a buffered next request, of a PL/pgSQL EXCEPTION
# block in a utility statement, and of pipelined Parse messages.

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
compute_query_id = on
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
    PERFORM pg_sleep(0.01);
    COMMIT;
    PERFORM pg_sleep(0.01);
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
# errors seen on stderr by the last b_finish()
my $b_errors = '';
my $run_cnt = 0;

# Send $sql and return a token to wait for.  Does not wait for completion.
sub b_start
{
	my ($sql) = @_;
	my $token = 'ORACLE_DONE_' . ++$run_cnt;
	$psql_b->{stderr} = '';
	$psql_b->{stdin} .= "$sql\n\\warn $token\n\\echo $token\n";
	# hand the input to psql now: the caller may poll before b_finish()
	$psql_b->{run}->pump_nb;
	return $token;
}

# Wait for completion, collect ora_id() notices.
sub b_finish
{
	my ($token) = @_;
	pump_until($psql_b->{run}, $psql_b->{timeout}, \$psql_b->{stdout},
		qr/$token/);
	pump_until($psql_b->{run}, $psql_b->{timeout}, \$psql_b->{stderr},
		qr/$token/);
	my $err = $psql_b->{stderr};
	$psql_b->{stdout} = '';
	$psql_b->{stderr} = '';
	while ($err =~ /NOTICE:\s+ORA:(\w+):(-?\d+)/g)
	{
		push @{ $ids{$1} }, $2;
	}
	$err =~ s/^.*NOTICE:.*\n//mg;
	$err =~ s/^.*$token.*\n?//mg;
	$b_errors = $err;
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
	select(undef, undef, undef, 0.2);
	$psql_a->query_safe('COMMIT;');
	b_finish($token);
}

# A statement with $k constant columns, which is what makes its query_id
# differ from every other statement's.  $from is an optional FROM clause.
sub sel
{
	my ($k, $from) = @_;
	return 'SELECT pg_sleep(0.01)' . (', 1' x $k) . ($from ? " $from" : '');
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
push @script, [ 'simple', 1, 1 ];

# one simple-query message, three statements (\; keeps them in one message)
explain_id('m1', sel(1));
explain_id('m2', sel(2));
explain_id('m3', sel(3));
b_run(sel(1) . " \;\n" . sel(2) . " \;\n" . sel(3) . ';');
push @script, [ 'm1', 1, 1 ], [ 'm2', 1, 1 ], [ 'm3', 1, 1 ];

# extended protocol, unnamed statement
explain_id('ext', sel_param(4), 1);
b_run(sel_param(4) . " \\bind 0.01 \\g");
push @script, [ 'ext', 1, 1 ];

# named prepared statement, three executions
explain_id('np', sel_param(5), 1);
b_run(sel_param(5) . " \\parse ora_np");
b_run("\\bind_named ora_np 0.01 \\g\n" x 3);
push @script, [ 'np', 3, 3 ];

# SQL PREPARE / EXECUTE: the bucket is the EXECUTE utility statement's
b_run('PREPARE ora_sp AS SELECT ora_id(\'sp\'), pg_sleep(0.01)'
	  . (', 1' x 6) . ';');
b_run('EXECUTE ora_sp;');
push @script, [ 'sp', 1, 1 ];

# pipeline with a sync between the executions: each execution commits,
# and the next one starts without an Idle in between
explain_id('pl', sel_param(7), 1);
b_run(sel_param(7) . " \\parse ora_pl");
b_run(  "\\startpipeline\n"
	  . "\\bind_named ora_pl 0.01 \\sendpipeline\n"
	  . "\\syncpipeline\n"
	  . "\\bind_named ora_pl 0.01 \\sendpipeline\n"
	  . "\\syncpipeline\n"
	  . "\\bind_named ora_pl 0.01 \\sendpipeline\n"
	  . "\\endpipeline");
push @script, [ 'pl', 3, 3 ];

# explicit transaction
explain_id('tx1', sel(8));
explain_id('tx2', sel(9));
b_run("BEGIN;\n" . sel(8) . ";\n" . sel(9) . ";\nCOMMIT;");
push @script, [ 'tx1', 1, 1 ], [ 'tx2', 1, 1 ];

# DO block, looping over a cached statement
b_run(  'DO $$ BEGIN PERFORM ora_id(\'do\'); '
	  . 'FOR i IN 1..3 LOOP PERFORM pg_sleep(0.01); END LOOP; END $$;');
push @script, [ 'do', 1, 3 ];

# CALL of a procedure that COMMITs inside, with a sleep on each side
b_run('CALL ora_proc();');
push @script, [ 'call', 1, 2 ];

# a statement that sleeps and then fails (the divisor comes from a table,
# so the error is raised during execution, after the sleep), then a normal
# statement in the same session
my $err_sql = sel(10, ', 1 / d FROM ora_zero');
explain_id('err', $err_sql);
b_run("$err_sql;");
like($b_errors, qr/division by zero/, 'the failing statement failed');
explain_id('after', sel(11));
b_run(sel(11) . ';');
push @script, [ 'err', 1, 1 ], [ 'after', 1, 1 ];

# lock wait during Bind of a named prepared statement: the statement was
# parsed before the lock was taken, so the wait is in plan cache
# revalidation, before the ExecStart that opens the statement
explain_id('lkb', sel_param(12, 'FROM ora_lock_t'), 1);
b_run(sel_param(12, 'FROM ora_lock_t') . " \\parse ora_lk");
b_run_blocked("\\bind_named ora_lk 0.01 \\g");
push @script, [ 'lkb', 1, 1 ];

# lock wait during parse analysis of a simple query, before QueryStart
explain_id('lks', sel(13, 'FROM ora_lock_t'));
b_run_blocked(sel(13, 'FROM ora_lock_t') . ';');
push @script, [ 'lks', 1, 1 ];

# lock_timeout expiring during Bind: no statement ever starts
b_run("SELECT count(*), 1, 2, 3 FROM ora_lock_t \\parse ora_lt");
b_run("SET lock_timeout = '100ms';");
$psql_a->query_safe("BEGIN;\nLOCK TABLE ora_lock_t IN ACCESS EXCLUSIVE MODE;");
b_run("\\bind_named ora_lt \\g");
$psql_a->query_safe('COMMIT;');
like($b_errors, qr/lock timeout/, 'Bind with an expiring lock_timeout fails');
b_run("RESET lock_timeout;");

# ---------------------------------------------------------------------
# Collect the expected ids and the function's output
# ---------------------------------------------------------------------
foreach my $tag (@from_notice)
{
	my @got = @{ $ids{$tag} || [] };
	is(scalar @got, 1, "$tag: reported its query_id once");
	$id{$tag} = $got[0];
}
my @tags = map { $_->[0] } @script;
is(scalar(keys %{ { map { $_ => 1 } values %id } }),
	scalar(@tags), 'the statements\' query_ids are pairwise distinct');
ok(!grep({ !$_ } values %id), '...and none of them is zero');

# B quiet: all its waits flushed to the ring
$node->poll_query_until(
	'postgres', qq(
	SELECT wait_event = 'ClientRead' FROM pg_stat_activity WHERE pid = $b_pid;
)) or die "backend $b_pid did not return to ClientRead";

my @rows;
foreach my $line (
	split /\n/,
	$node->safe_psql(
		'postgres', qq(
		SELECT bucket, statement_seq, query_id, phase, wait_event_type,
		       wait_event, calls
		FROM pg_wait_event_trace_by_statement($b_pn);
	)))
{
	my @f = split /\|/, $line, -1;
	push @rows,
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

# rows of statement $tag for $type:$event
sub rows_of
{
	my ($tag, $type, $event) = @_;
	return grep {
		defined $_->{qid}
		  && $_->{qid} eq $id{$tag}
		  && $_->{type} eq $type
		  && $_->{event} eq $event
	} @rows;
}

# ---------------------------------------------------------------------
# Every statement's sleep is in its own bucket(s), in script order
# ---------------------------------------------------------------------
my $prev_max = -1;
foreach my $s (@script)
{
	my ($tag, $execs, $min_calls) = @$s;
	my @r = rows_of($tag, 'Timeout', 'PgSleep');
	my %seqs = map { $_->{seq} => 1 } @r;
	my $sleeps = 0;
	$sleeps += $_->{calls} foreach @r;

	is(scalar keys %seqs, $execs,
		"$tag: PgSleep waits are in exactly $execs statement bucket(s)");
	cmp_ok($sleeps, '>=', $min_calls,
		"$tag: at least $min_calls PgSleep call(s) in them");
	is(join(',', sort { $a cmp $b } map { $_->{phase} } @r),
		join(',', ('statement') x scalar @r),
		"$tag: ...in phase 'statement'");

	my ($min, $max) = (sort { $a <=> $b } keys %seqs)[ 0, -1 ];
	cmp_ok($min // -1, '>', $prev_max,
		"$tag: its bucket(s) come after those of the previous statement");
	$prev_max = $max // $prev_max;
}

# Waits before the start marker: phase 'pre' of the statement's bucket
foreach my $tag (qw(lkb lks))
{
	my @r = rows_of($tag, 'Lock', 'relation');
	ok(@r == 1 && $r[0]{phase} eq 'pre' && $r[0]{calls} >= 1,
		"$tag: Lock:relation wait is in the statement's bucket, phase pre");
}

# lock_timeout during Bind: the wait is in <no-statement>, which has
# no statement_seq, query_id or phase
my @nostmt = grep { $_->{bucket} eq '<no-statement>' } @rows;
ok(@nostmt >= 1 && !grep({ $_->{type} ne 'Lock' || $_->{event} ne 'relation' }
		@nostmt),
	'lock_timeout during Bind: Lock:relation wait is in <no-statement>');
ok( !grep({ defined $_->{seq} || defined $_->{qid} || $_->{phase} ne '' }
		@nostmt),
	'...which carries no statement_seq, query_id or phase');

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

is( $node->safe_psql(
		'postgres', qq(
		SELECT (SELECT sum(calls)
		        FROM pg_wait_event_trace_by_statement($b_pn))
		     = (SELECT count(*) FROM pg_get_wait_event_trace($b_pn)
		        WHERE wait_event_type <> 'Query');
	)),
	't',
	'sum(calls) equals the number of non-marker records of the ring');

$psql_a->quit;
$psql_b->quit;
$node->stop;
done_testing();
