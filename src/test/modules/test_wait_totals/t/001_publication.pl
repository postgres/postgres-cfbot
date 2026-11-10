# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: counting and the publication
# rule.  Every assertion is about conservation of one dedicated wait event
# (TestWaitTotals, emitted by test_wait_totals), not about "all totals":
# with cluster-wide capture, startup and background waits keep adding to
# every other event.
#
# 1. Basic: N emitted waits become N calls with a positive total time, once
#    the emitting backend has returned to a timed wait.
# 2. Publication rule: a completed wait is added to the ledger when the next
#    timed wait begins (or another explicit flush happens).  After N
#    consecutive emissions with no flush in between, N-1 are published and
#    one remains private; a statement end in stats mode does not flush, the
#    executor end in trace mode does.  A later timed wait publishes the rest.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use PostgreSQL::Test::Cluster;
use TwtUtil;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
TwtUtil::harden($node);
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.capture = 'stats'
debug_parallel_query = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

sub calls
{
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'");
}

sub time_ms
{
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(total_time_ms), 0) FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'");
}

# Keep the value constant for a moment: "exactly N" must still hold later.
sub stays
{
	my ($want, $what) = @_;
	usleep(500_000);
	is(calls(), $want, $what);
}

# ---------------------------------------------------------------------
# 1. Basic.
# ---------------------------------------------------------------------
is(calls(), '0', 'dedicated event starts at zero');

my $a = $node->background_psql('postgres');
$a->query_safe('SELECT test_wait_totals_emit(10, 20)');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = 10 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'N emissions did not become N calls';
stays('10', 'calls delta is exactly N');
cmp_ok(time_ms(), '>', 0, 'total time is positive');
cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT avg_time_us FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'"),
	'>=', 20, 'average time is at least the spin time');
$a->quit;

# ---------------------------------------------------------------------
# 2. Publication rule, observed from another session while the emitter is
#    inside its CPU stretch.  The emitter holds there until released, and
#    tells us when it is there, so the observation provably happens inside
#    the stretch, whatever the scheduling.
# ---------------------------------------------------------------------
# $mode: capture level of the emitting session; $stmt: what it runs;
# $expect: calls visible while it holds, for N = 5 emissions.
sub publication_case
{
	my ($mode, $stmt, $expect, $label) = @_;
	my $base = calls();
	my $s = $node->background_psql('postgres');

	$s->query_safe("SET pg_wait_event_tracing.capture = $mode");

	# Warm up the statement text with the gate open, so that the measured
	# run does not hit catalog reads (a timed IO wait would flush the
	# pending completion).
	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	$s->query_safe($stmt);
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $base + 5 FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "$label: warm-up not published";
	$base += 5;

	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
	$s->query_until(qr/started/, "\\echo started\n$stmt\n");
	$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
	  or die "$label: the emitter never reached its CPU stretch";

	# One read, while the emitter is known to be inside the stretch.
	is(calls(), $base + $expect,
		"$label: $expect published while the backend is in its CPU stretch");

	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	# Ending the statement returns the backend to a timed wait, which
	# publishes the one that remained private.
	$s->query_safe('SELECT 1');
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $base + 5 FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "$label: the private completion was never published";
	stays($base + 5, "$label: all 5 published after the next timed wait");
	$s->quit;
}

publication_case('stats',
	'SELECT test_wait_totals_emit_hold(5);',
	4, 'stats, one call');
publication_case('trace',
	'SELECT test_wait_totals_emit_hold(5);',
	4, 'trace, one call');
# psql sends "a \; b" as one query string, so no timed wait (ClientRead)
# separates the two statements.  The first ends before the second holds.
publication_case('stats',
	'SELECT test_wait_totals_emit(5) \; SELECT test_wait_totals_emit_hold(0);',
	4, 'stats, statement end does not flush');
publication_case('trace',
	'SELECT test_wait_totals_emit(5) \; SELECT test_wait_totals_emit_hold(0);',
	5, 'trace, statement markers flush');

# ---------------------------------------------------------------------
# Names: the unnamed built-in Extension event is "Extension"; an event id
# that nothing registered gets a placeholder name, never an error.
# ---------------------------------------------------------------------
my $ext = 117440512;    # PG_WAIT_EXTENSION, 0x07000000
$node->safe_psql('postgres', "SELECT test_wait_totals_emit_event($ext, 2)");
$node->safe_psql('postgres', "SELECT test_wait_totals_emit_event($ext + 120, 1)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 2 FROM pg_stat_wait_event_totals
	 WHERE (event_key = $ext AND wait_event = 'Extension' AND calls = 2)
	    OR (event_key = $ext + 120 AND wait_event = 'unknown_120' AND calls = 1)")
  or die 'Extension / placeholder names are wrong: '
  . $node->safe_psql('postgres',
	"SELECT string_agg(event_key || ':' || wait_event, ',') FROM pg_stat_wait_event_totals
	 WHERE wait_event_type = 'Extension'");
pass('built-in Extension event and an unregistered id are named');

$node->stop;
done_testing();
