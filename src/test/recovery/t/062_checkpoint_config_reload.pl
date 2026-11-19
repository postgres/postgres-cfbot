
# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q[
autovacuum = off
log_checkpoints = on
log_min_messages = debug2
]);
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1, 100000) AS a');

my $logstart = -s $node->logfile;
$node->safe_psql('postgres',
	"SELECT injection_points_attach('checkpoint-write-delay', 'wait')");
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_until(
	qr/starting_checkpoint/, q(
\echo starting_checkpoint
CHECKPOINT;
));
$node->wait_for_event('checkpointer', 'checkpoint-write-delay');
ok($node->log_contains('checkpoint starting: fast', $logstart),
	'fast checkpoint has started');

sub change_during_checkpoint
{
	my ($value) = @_;
	my $offset = -s $node->logfile;

	$node->safe_psql('postgres',
		"ALTER SYSTEM SET synchronous_standby_names = '$value'");
	$node->reload;
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('checkpoint-write-delay')");
	$node->wait_for_log(
		qr/checkpointer updated shared memory configuration values/, $offset);
	ok( !$node->log_contains('checkpoint complete', $logstart),
		"synchronous_standby_names = '$value' applied before the checkpoint completed"
	);
}

change_during_checkpoint('*');
my $commit = $node->background_psql('postgres');
$commit->query_until(
	qr/committing/, q(
\echo committing
INSERT INTO t VALUES (0);
));
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'SyncRep'"
	),
	'commit waits for the standby while the checkpoint is running');

change_during_checkpoint('');
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'SyncRep'"
	),
	'waiting commit is released while the checkpoint is running');
$commit->quit;

ok(!$node->log_contains('checkpoint complete', $logstart),
	'checkpoint is still running');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('checkpoint-write-delay')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('checkpoint-write-delay')");
$checkpoint->quit;
ok($node->log_contains('checkpoint complete: fast', $logstart),
	'checkpoint has completed');

$node->stop;

done_testing();
