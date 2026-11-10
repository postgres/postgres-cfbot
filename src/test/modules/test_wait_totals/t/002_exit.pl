# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: a completed wait that is still
# pending when its process exits reaches the totals.  Disconnecting after a
# successful SQL result would only test an already-flushed wait, so the cases
# here make sure the last completion really is pending at exit:
#
#  - a background worker emits N waits and exits without returning to
#    any wait or to SQL (a server-side process that records through the
#    reserved fixed-slot region and never attaches anything);
#  - a client backend is terminated while it burns CPU after N emissions
#    (the FATAL exit path);
#  - an idle disconnect, as smoke coverage only.

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

sub wait_for_calls
{
	my ($want, $what) = @_;
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $want FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "$what: calls never reached $want (now " . calls() . ")";
}

# ---------------------------------------------------------------------
# Background worker: emit N, exit.  The call returns after the worker has
# exited, so the count must be exact right away, without polling.
# ---------------------------------------------------------------------
my $a = $node->background_psql('postgres');
$a->query_safe('SELECT test_wait_totals_event_key()');    # register the name


$a->query_safe('SELECT test_wait_totals_bgworker(7)');
is(calls(), '7', 'bgworker: all 7 completions counted, the last one included');

$a->query_safe('SELECT test_wait_totals_bgworker(1)');
is(calls(), '8', 'bgworker: a single completion, pending at exit, counted');

# A fixed-slot worker withdraws its owner token at an orderly exit (before its
# PGPROC is released), so an exited worker is not a capturing backend.
# The population of the instance-wide count is not controllable, so the test
# looks at the worker's own slot through an assert-build test entry point
# that applies the same liveness rule as capturing_backends.
SKIP:
{
	skip 'test entry point exists only in assert-enabled builds', 5
	  unless check_pg_config('#define USE_ASSERT_CHECKING 1');

	$node->safe_psql('postgres',
		q(CREATE FUNCTION pwet_test_slot_counted(int) RETURNS bool
		  AS 'pg_wait_event_tracing', 'pwet_test_slot_counted' LANGUAGE C));
	my $wslot = $a->query_safe('SELECT test_wait_totals_bgworker_procnumber()');
	cmp_ok($wslot, '>=', 0, "the worker ran in slot $wslot");
	is($a->query_safe("SELECT pwet_test_slot_counted($wslot)"), 'f',
		'an exited fixed-slot worker is not a capturing backend');

	# A worker without a database connection has no backend-status entry; it
	# is still a capturing backend while it is alive, and not after it exits.
	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
	$a->query_safe('SELECT test_wait_totals_bgworker_hold(3)');
	$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
	  or die 'the held worker never reached its loop';
	my $hslot = $a->query_safe('SELECT test_wait_totals_bgworker_procnumber()');
	is($a->query_safe("SELECT pwet_test_slot_counted($hslot)"), 't',
		'a live fixed-slot worker without a backend-status entry is counted');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	$node->poll_query_until('postgres',
		"SELECT NOT pwet_test_slot_counted($hslot)")
	  or die 'the worker is still counted after it exited';
	pass('and it is not counted after it exited');
	is($a->query_safe('SELECT pwet_test_slot_counted(test_wait_totals_procnumber())'),
		't', 'a live capturing backend is');
}

my $before_zero = calls();
$a->query_safe('SELECT test_wait_totals_bgworker(0)');
is(calls(), $before_zero, 'bgworker: emitting nothing counts nothing');
$a->quit;

# ---------------------------------------------------------------------
# Client backend terminated while burning CPU after N emissions: N-1 are
# published, the last is pending, and the FATAL exit must drain it.
# ---------------------------------------------------------------------
my $base = calls();
my $s = $node->background_psql('postgres');
my $pid = $s->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
$s->query_until(qr/started/,
	"\\echo started\nSELECT test_wait_totals_emit_hold(6);\n");
$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
  or die 'the emitter never reached its CPU stretch';
is(calls(), $base + 5,
	'terminate: exactly N-1 published while the backend is in its CPU stretch');

$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
wait_for_calls($base + 6, 'terminate: the pending completion drained at exit');
usleep(500_000);
is(calls(), $base + 6, 'terminate: exactly N after termination, none twice');
eval { $s->quit };

# ---------------------------------------------------------------------
# The producer is cancelable: a huge emission (each pair spinning a full
# second) ends with the cancellation error soon after the cancel.
# ---------------------------------------------------------------------
{
	my $c = $node->background_psql('postgres');
	my $cpid = $c->query_safe('SELECT pg_backend_pid()');
	$c->query_until(qr/emitting/, "\\echo emitting\nSELECT test_wait_totals_emit(1000000000, 1000000);\n");
	$node->poll_query_until('postgres',
		"SELECT state = 'active' FROM pg_stat_activity WHERE pid = $cpid")
	  or die 'the emission never started';
	my $off = -s $node->logfile;
	$node->safe_psql('postgres', "SELECT pg_cancel_backend($cpid)");
	$node->wait_for_log(
		qr/ERROR:  canceling statement due to user request\n.*?STATEMENT:  SELECT test_wait_totals_emit\(1000000000, 1000000\)/s,
		$off);
	pass('a huge emission is canceled with the cancellation error');
	# psql exits on the error, so the backend is gone or idle; not running.
	$node->poll_query_until('postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $cpid AND state = 'active')")
	  or die 'the canceled emission is still running';
	pass('and it is no longer running');
	eval { $c->quit };
}

# ---------------------------------------------------------------------
# Idle disconnect: smoke only (the completion is flushed long before).
# ---------------------------------------------------------------------
$base = calls();
$node->safe_psql('postgres', 'SELECT test_wait_totals_emit(3)');
wait_for_calls($base + 3, 'idle disconnect');
is(calls(), $base + 3, 'idle disconnect: all 3 counted');

$node->stop;
done_testing();
