# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: slot reuse, server processes and
# parallel workers.
#
# 6. Many short connections reuse ProcNumbers (the free list is first-in
#    first-out, so max_connections is kept small to make it wrap around).  The ledger of a slot is never
#    cleared by a new owner, so the dedicated event's total equals everything
#    emitted, and the test checks that reuse really happened.
# 7. With capture = stats cluster-wide, the checkpointer and the WAL writer
#    contribute, asserted on events only they wait on (CheckpointerMain,
#    WalWriterMain), not on a generic WAL write a backend could do as well.
# 8. Parallel workers that emit through a forced parallel plan are counted
#    once each after they have exited, and the test shows that the workers,
#    not the leader, really ran the emitter.

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
wal_writer_delay = 10ms
max_connections = 12
max_worker_processes = 16
max_parallel_workers = 8
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

sub event_calls
{
	my ($event) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
		 WHERE wait_event = '$event'");
}

sub calls { return event_calls('TestWaitTotals'); }

# ---------------------------------------------------------------------
# 6. Slot reuse.
# ---------------------------------------------------------------------
my $conns = 30;
my $per_conn = 3;
my %seen;
my @order;
for my $i (1 .. $conns)
{
	my $out = $node->safe_psql('postgres',
		"SELECT test_wait_totals_procnumber(), test_wait_totals_emit($per_conn)");
	my ($procno) = split /\|/, $out;
	$seen{$procno}++;
	push @order, $procno;
}
my $distinct = scalar keys %seen;
cmp_ok($distinct, '<', $conns,
	"slots were reused: $distinct distinct ProcNumbers for $conns connections ("
	  . join(',', @order) . ')');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = " . ($conns * $per_conn) . "
	 FROM pg_stat_wait_event_totals WHERE wait_event = 'TestWaitTotals'")
  or die 'slot reuse: totals never reached the emitted count';
usleep(300_000);
is(calls(), $conns * $per_conn,
	'slot reuse: total equals everything emitted, no ledger was cleared');

# ---------------------------------------------------------------------
# 7. Server processes.
# ---------------------------------------------------------------------
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) >= 20 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'WalWriterMain'")
  or die 'the WAL writer never contributed WalWriterMain waits';
pass('WAL writer contributes its own WalWriterMain waits');

my $ckpt_before = event_calls('CheckpointerMain');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) > $ckpt_before FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'CheckpointerMain'")
  or die 'the checkpointer never contributed after a CHECKPOINT';
pass('checkpointer contributes CheckpointerMain waits after a CHECKPOINT');

# ---------------------------------------------------------------------
# 8. Parallel workers.
# ---------------------------------------------------------------------
my $rows = 4000;
$node->safe_psql('postgres',
	"CREATE TABLE ptab AS SELECT g FROM generate_series(1, $rows) g");

my $setup = q(
SET debug_parallel_query = off;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
);
my $query = 'SELECT sum(test_wait_totals_emit_row(1)) FROM ptab';

my $plan = $node->safe_psql('postgres', "${setup}EXPLAIN (COSTS OFF) $query");
like($plan, qr/Gather/, 'the emitter runs under a parallel plan');

my $base = calls();
my $in_workers = $node->safe_psql('postgres', "$setup$query");
is($in_workers, $rows,
	'every row was processed by a worker, none by the leader');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = $base + $rows FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'parallel workers: totals never reached the emitted count (now '
  . calls() . ", base $base)";
usleep(300_000);
is(calls(), $base + $rows,
	"parallel workers: $rows completions counted exactly once after they exited");

$node->stop;
done_testing();
