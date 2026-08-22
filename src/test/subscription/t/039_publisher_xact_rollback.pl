# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that pg_stat_database.xact_rollback on a logical-replication
# publisher is not inflated by the walsender's internal catalog-cleanup
# aborts.  ReorderBufferProcessTXN() ends each decoded transaction with
# AbortCurrentTransaction(); in the walsender that is a top-level abort.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub get_xact_stats
{
	my ($node) = @_;

	return split /\|/, $node->safe_psql('template1', q{
		SELECT xact_commit, xact_rollback
		FROM pg_stat_database
		WHERE datname = 'postgres'
	});
}

# Wait for this subscription's walsender to disappear from pg_stat_activity.
#
# A walsender registers its stats flush (pgstat_shutdown_hook) with
# before_shmem_exit() and clears its pg_stat_activity entry
# (pgstat_beshutdown_hook) with on_shmem_exit().  shmem_exit() runs all
# before_shmem_exit callbacks before any on_shmem_exit callback, so by the time
# the walsender is gone from pg_stat_activity its xact_commit/xact_rollback
# counts have already been flushed to shared stats.  Observing its exit is
# therefore sufficient synchronization for reading the flushed counters.
sub wait_for_walsender_exit
{
	my ($node) = @_;

	$node->poll_query_until(
		'template1', q{
		SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'walsender' AND application_name = 's'
	})
	  or die 's walsender did not exit';
}

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
# Autovacuum would commit on the postgres database and perturb the xact_commit
# delta this test compares between the control and decoding runs.
$node_publisher->append_conf('postgresql.conf', 'autovacuum = off');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

$node_publisher->safe_psql('postgres',
	'CREATE TABLE t (id int PRIMARY KEY)');
$node_subscriber->safe_psql('postgres',
	'CREATE TABLE t (id int PRIMARY KEY)');

$node_publisher->safe_psql('postgres', 'CREATE PUBLICATION p FOR TABLE t');

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION s CONNECTION '$publisher_connstr' PUBLICATION p");

$node_subscriber->wait_for_subscription_sync($node_publisher, 's');

# Measure a walsender shutdown without any decoded committed transactions.
# The absolute xact_commit delta is not asserted: walsender shutdown performs
# fixed session/transaction bookkeeping that may change independently of this
# test.  The control run captures that non-decoding delta so the decoding run
# can assert it is unchanged by decoded transactions.
my ($control_base_commit, $control_base_rollback) =
  get_xact_stats($node_publisher);

$node_subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION s DISABLE');
wait_for_walsender_exit($node_publisher);

my ($control_final_commit, $control_final_rollback) =
  get_xact_stats($node_publisher);

my $control_commit_delta = $control_final_commit - $control_base_commit;
my $control_rollback_delta = $control_final_rollback - $control_base_rollback;

$node_subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION s ENABLE');

# Five autocommit INSERTs: each becomes one decoded committed transaction on
# the walsender.  Without the fix, those produce five spurious rollbacks after
# DISABLE.  xact_commit may change due to fixed walsender bookkeeping, but it
# must not change as a function of decoded transactions.
my $n = 5;
$node_publisher->safe_psql('postgres',
	join('', map { "INSERT INTO t VALUES ($_);\n" } 1 .. $n));

$node_publisher->wait_for_catchup('s');

# Baseline after catchup and before walsender exit, so the deltas measure only
# the stats flushed by this subscription's walsender during shutdown.
my ($base_commit, $base_rollback) = get_xact_stats($node_publisher);

# Disabling the subscription terminates the walsender; its shutdown hook
# flushes pgstat counters to shared stats before it leaves pg_stat_activity.
$node_subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION s DISABLE');
wait_for_walsender_exit($node_publisher);

my ($final_commit, $final_rollback) = get_xact_stats($node_publisher);

cmp_ok(
	$control_rollback_delta, '==', 0,
	'walsender shutdown without decoded transactions does not inflate publisher xact_rollback'
);

cmp_ok(
	$final_rollback - $base_rollback, '==', 0,
	'walsender does not inflate publisher xact_rollback for decoded transactions'
);

cmp_ok(
	$final_commit - $base_commit, '==', $control_commit_delta,
	'walsender does not change publisher xact_commit as a function of decoded transactions'
);

done_testing();
