# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: snapshots, diffs and the report
# on a live server.  Crafted-document validation (schema version, integer
# ranges, unique keys, ordering, incarnation, generation, argument checks and
# permissions) is in the module's regress test; here real snapshots and the
# dedicated event give exact numbers.  Capture is off cluster-wide; the one
# worker session turns it on, so the only transactions counted are its own.
#
#  - ratios: 10 waits in 2 transactions give waits_per_xact 5 and the
#    matching ms ratios;
#  - zero denominator: with the emitter held inside its transaction, the
#    transaction count of the interval is zero and the ratios are NULL;
#  - include_idle: Activity and Client/ClientRead rows only when asked for,
#    the transaction denominator unchanged;
#  - an event missing from the second snapshot is an error, one missing from
#    the first counts from zero;
#  - the report: the NOTICE of an idle interval (the interval is checked
#    against a lower bound only), the real pg_sleep path being cancelable, and
#    argument errors.  What a report contains is the diff of two snapshots,
#    which the checks above exercise deterministically; no test depends on
#    activity landing inside a report's sleep.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use PostgreSQL::Test::Cluster;
use TwtUtil;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
TwtUtil::harden($node);
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
autovacuum = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

# One long-lived observer session does all the queries of this test, instead
# of a short-lived psql per query: on macOS /usr/bin/perl 5.34 can crash in its
# signal handler when a SIGCHLD arrives on a foreign thread, and every psql
# child that exits is such a signal.
my $obs = $node->background_psql('postgres', on_error_stop => 0);
$obs->set_query_timer_restart();    # the timer bounds each query, not the session

sub oq
{
	my ($sql) = @_;
	$obs->{stderr} = '';
	return $obs->query_safe($sql, verbose => 0);
}

# Run a statement that may fail or notice: (stdout, stderr).
sub oerr
{
	my ($sql) = @_;
	$obs->{stderr} = '';
	my $out = $obs->query($sql, verbose => 0);
	my $err = $obs->{stderr};
	$obs->{stderr} = '';
	return ($out, $err);
}

# Poll with a query on the observer; the default expected value is 't'.
sub opoll
{
	my ($sql, $want) = @_;
	$want //= 't';
	my $deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
	my $last = '';
	while (time() < $deadline)
	{
		$last = oq($sql);
		return 1 if $last eq $want;
		select(undef, undef, undef, 0.1);
	}
	diag("opoll timed out: $sql\nlast: $last");
	return 0;
}

sub snapshot { return oq('SELECT pg_wait_event_snapshot()'); }

sub lit { my ($j) = @_; return "\$pwet\$$j\$pwet\$::jsonb"; }

sub diff
{
	my ($a, $b, $idle, $cols) = @_;
	$idle //= 'false';
	$cols //= '*';
	return oq(
		"SELECT $cols FROM pg_wait_event_diff(" . lit($a) . ', ' . lit($b) . ", $idle) d
		 WHERE wait_event = 'TestWaitTotals' OR event_key IN (83886080, 100663296)
		 ORDER BY event_key");
}

# ---------------------------------------------------------------------
# The report with nothing happening: no rows, but the NOTICE has the
# interval and the transactions.  The interval is only bounded from below.
# ---------------------------------------------------------------------
{
	my ($stdout, $stderr) = oerr('SELECT count(*) FROM pg_wait_event_report(1)');
	is($stdout, '0', 'report of an idle server has no rows');
	like($stderr,
		qr/NOTICE:  interval (\d+\.\d+) s, 0 transactions, 0\.00 transactions per second/,
		'but its NOTICE has the interval and the transactions');
	my ($interval) = $stderr =~ /interval (\d+\.\d+) s/;
	cmp_ok($interval, '>=', 1, 'the reported interval is at least the requested second');
}

# ---------------------------------------------------------------------
# The real pg_sleep path is cancelable.
# ---------------------------------------------------------------------
{
	# on_error_stop off: the session survives the cancellation error, so it
	# is synchronized with a plain query instead of a vanished psql.
	my $b = $node->background_psql('postgres', on_error_stop => 0);
	$b->set_query_timer_restart();
	my $bpid = $b->query_safe('SELECT pg_backend_pid()', verbose => 0);
	$b->query_until(qr/reporting/, "\\echo reporting\nSELECT * FROM pg_wait_event_report(600);\n");
	opoll("SELECT wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid = $bpid")
	  or die 'the report never started sleeping';
	my $off = -s $node->logfile;
	oq("SELECT pg_cancel_backend($bpid)");
	# The cancellation error itself, for that statement, in the server log
	# and on the session.
	$node->wait_for_log(
		qr/ERROR:  canceling statement due to user request\n.*?STATEMENT:  SELECT \* FROM pg_wait_event_report\(600\)/s,
		$off);
	my $one = $b->query('SELECT 1', verbose => 0);
	my $berr = $b->{stderr};
	$b->{stderr} = '';
	is($one, '1', 'the canceled session is still usable');
	like($berr, qr/canceling statement due to user request/,
		'a report sleeping in pg_sleep is canceled with the cancellation error');
	$b->quit;
}

# Argument errors.
{
	my ($out, $err) = oerr('SELECT * FROM pg_wait_event_report(0)');
	like($err, qr/ERROR:  seconds must be at least 1/, 'seconds < 1 is rejected with an ERROR');
}

# ---------------------------------------------------------------------
# Ratios.
# ---------------------------------------------------------------------
my $w = $node->background_psql('postgres');
$w->set_query_timer_restart();
$w->query_safe('SET pg_wait_event_tracing.capture = stats');
$w->query_safe('SELECT test_wait_totals_event_key()');

my $s_a = snapshot();
$w->query_safe('SELECT test_wait_totals_emit(4, 50)');
$w->query_safe('SELECT test_wait_totals_emit(6, 50)');
opoll(
	"SELECT coalesce(sum(calls), 0) = 10 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'the emissions were never published';
my $s_b = snapshot();

is(diff($s_a, $s_b, 'false',
		'waits, xacts, waits_per_xact, abs(ms_per_xact * xacts - wait_time_ms) < 1e-9, '
		. 'abs(ms_per_wait * waits - wait_time_ms) < 1e-9, interval_s > 0, wait_time_ms > 0'),
	'10|2|5|t|t|t|t',
	'10 waits in 2 transactions: waits_per_xact 5, ms ratios consistent');

# ---------------------------------------------------------------------
# idle filter.
# ---------------------------------------------------------------------
my $s_c = snapshot();
$w->query_safe('SELECT test_wait_totals_emit_event(83886080, 2)');       # Activity
$w->query_safe('SELECT test_wait_totals_emit_event(100663296, 3)');      # Client:ClientRead
$w->query_safe('SELECT test_wait_totals_emit(1)');
opoll(
	"SELECT coalesce(sum(calls), 0) = 11 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'the emissions were never published';
my $s_d = snapshot();
is(diff($s_c, $s_d, 'false', 'wait_event'), 'TestWaitTotals',
	'include_idle = false: Activity and ClientRead are left out');
like(diff($s_c, $s_d, 'true', 'event_key'), qr/^83886080\n100663296\n/,
	'include_idle = true: they are there');
is((split /\n/, diff($s_c, $s_d, 'false', 'xacts'))[0],
	(split /\n/, diff($s_c, $s_d, 'true', 'xacts'))[0],
	'the transaction denominator is not filtered');
like(diff($s_c, $s_d, 'true', 'waits'), qr/^2\n/,
	'the Activity row has the emitted 2 waits (and the others follow)');

# ---------------------------------------------------------------------
# Missing events.
# ---------------------------------------------------------------------
my $later = oq(
	"SELECT jsonb_set(jsonb_set(jsonb_set(" . lit($s_d) . ", '{events}', '[]'),
	    '{scan_start}', to_jsonb((" . lit($s_d) . "->>'scan_end')::numeric + 1)),
	    '{scan_end}', to_jsonb((" . lit($s_d) . "->>'scan_end')::numeric + 2))");
my ($out, $err) = oerr('SELECT * FROM pg_wait_event_diff(' . lit($s_d) . ', ' . lit($later) . ')');
like($err, qr/ERROR:  event \d+ of the first snapshot is missing from the second/,
	'an event missing from the second snapshot is an ERROR');

# Missing from the first counts from zero: s_a had no dedicated event row.
unlike($s_a, qr/TestWaitTotals/, 'the first snapshot has no dedicated event');
is(diff($s_a, $s_b, 'false', 'waits'), '10', 'an event only in the second counts from zero');

# ---------------------------------------------------------------------
# Zero denominator: the emitter is held inside its transaction, so three
# completions are published while no transaction completed in the interval.
# ---------------------------------------------------------------------
oq( 'SELECT test_wait_totals_gate_reset()');
my $s_e = snapshot();
$w->query_until(qr/started/,
	"\\echo started\nSELECT test_wait_totals_emit_hold(4);\n");
opoll( 'SELECT test_wait_totals_holding()')
  or die 'the emitter never reached its CPU stretch';
my $s_f = snapshot();
is(diff($s_e, $s_f, 'false',
		'waits, xacts, waits_per_xact IS NULL, ms_per_xact IS NULL, ms_per_wait IS NOT NULL'),
	'3|0|t|t|t', 'no transaction in the interval: the per-transaction ratios are NULL');
oq( 'SELECT test_wait_totals_release()');
$w->query_safe('SELECT 1');
$w->quit;

# ---------------------------------------------------------------------
# A report with rows, deterministically (assert builds): the first snapshot
# of the report is suspended right after its scan, activity is produced and
# published while it is suspended, then the report continues.  So the
# activity is certainly between the two snapshots, with no sleep window to
# hit.  Also checks that include_idle is forwarded to the diff.
# ---------------------------------------------------------------------
if (check_pg_config('#define USE_ASSERT_CHECKING 1'))
{
	oq( q(
CREATE FUNCTION pwet_test_read_barrier_arm() RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_arm' LANGUAGE C;
CREATE FUNCTION pwet_test_read_barrier_waiting() RETURNS bool
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_waiting' LANGUAGE C;
CREATE FUNCTION pwet_test_read_barrier_release() RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_release' LANGUAGE C;
));
	my $act = $node->background_psql('postgres');
	$act->set_query_timer_restart();
	$act->query_safe('SET pg_wait_event_tracing.capture = stats');
	$act->query_safe('SELECT test_wait_totals_event_key()');

	foreach my $idle ('false', 'true')
	{
		my $base = oq(
			"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
			 WHERE wait_event = 'TestWaitTotals'");
		oq( 'SELECT pwet_test_read_barrier_arm()');
		my $rb = $node->background_psql('postgres');
		$rb->set_query_timer_restart();
		$rb->query_safe('SET client_min_messages = warning');    # the NOTICE is tested above
		$rb->query_until(qr/reporting/, qq(\\echo reporting
CREATE TEMP TABLE rep AS SELECT * FROM pg_wait_event_report(1, $idle);
));
		opoll( 'SELECT pwet_test_read_barrier_waiting()')
		  or die 'the report never reached its first snapshot barrier';

		# The report holds its first scan; this activity comes after it.
		$act->query_safe('SELECT test_wait_totals_emit(7)');
		$act->query_safe('SELECT test_wait_totals_emit_event(83886080, 2)');    # Activity
		opoll(
			"SELECT coalesce(sum(calls), 0) = $base + 7 FROM pg_stat_wait_event_totals
			 WHERE wait_event = 'TestWaitTotals'")
		  or die 'the activity was never published';
		oq( 'SELECT pwet_test_read_barrier_release()');
		$rb->query_safe('SELECT 1');    # the report is over

		is($rb->query_safe(q(SELECT waits || ',' || xacts FROM rep WHERE wait_event = 'TestWaitTotals')),
			'7,2', "report (include_idle = $idle): the 7 waits of 2 transactions, between its snapshots");
		is($rb->query_safe(q(SELECT count(*) FROM rep WHERE event_key = 83886080)),
			($idle eq 'true' ? '1' : '0'),
			"report (include_idle = $idle): the Activity row is " . ($idle eq 'true' ? 'there' : 'left out'));
		$rb->quit;
	}
	$act->quit;
}

$obs->quit;
$node->stop;
done_testing();
