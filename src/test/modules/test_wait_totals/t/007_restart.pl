# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing across restarts.  The ledgers live
# in shared memory, so they start from zero whenever the shared memory is
# created: after a clean restart and after a crash restart (kill -9 of a
# backend).  Each time the incarnation changes, which is how a consumer tells
# "the totals restarted" from "the totals only moved".  With cluster-wide
# capture, startup and background waits add to other events, so the
# assertions are on the dedicated event.

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
pg_wait_event_tracing.capture = 'stats'
debug_parallel_query = off
restart_after_crash = on
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

sub wait_calls
{
	my ($want) = @_;
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $want FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "calls never reached $want (now " . calls() . ')';
}

sub incarnation
{
	return $node->safe_psql('postgres',
		'SELECT incarnation FROM pg_stat_wait_event_totals_info');
}

my %seen;

$node->safe_psql('postgres', 'SELECT test_wait_totals_emit(5)');
wait_calls(5);
my $inc = incarnation();
like($inc, qr/^[0-9a-f]{16}$/, 'incarnation is 16 hex digits');
is(incarnation(), $inc, 'incarnation is stable while the server runs');
$seen{$inc} = 1;

# Clean restart.
$node->restart;
my $inc2 = incarnation();
ok(!$seen{$inc2}, 'clean restart: new incarnation');
$seen{$inc2} = 1;
is(calls(), '0', 'clean restart: the dedicated event starts from zero');
$node->safe_psql('postgres', 'SELECT test_wait_totals_emit(3)');
wait_calls(3);
is(calls(), '3', 'clean restart: counting works again, from zero');

# Crash restart: kill -9 of a client backend.  The postmaster reinitializes
# shared memory and restarts.
my $victim = $node->background_psql('postgres');
my $pid = $victim->query_safe('SELECT pg_backend_pid()');
ok(kill('KILL', $pid), 'SIGKILL sent to a backend');

# Connections may fail while the postmaster recovers, and may even succeed
# against the old instance before it notices the death: accept only a
# successful query that shows a different incarnation.
my $inc3;
for (my $i = 0; $i < 1800; $i++)
{
	my ($ret, $out, $err) = $node->psql('postgres',
		'SELECT incarnation FROM pg_stat_wait_event_totals_info');
	if ($ret == 0 && $out =~ /^[0-9a-f]{16}$/ && $out ne $inc2)
	{
		$inc3 = $out;
		last;
	}
	select(undef, undef, undef, 0.1);
}
die 'the server did not come back with a new incarnation' unless defined $inc3;
eval { $victim->quit };

ok(!$seen{$inc3}, 'crash restart: new incarnation');
$seen{$inc3} = 1;
is(calls(), '0', 'crash restart: the dedicated event starts from zero');
$node->safe_psql('postgres', 'SELECT test_wait_totals_emit(2)');
wait_calls(2);
is(calls(), '2', 'crash restart: counting works again, from zero');

$node->stop;
done_testing();
