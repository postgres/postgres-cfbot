# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_rewind rewinds a cleanly stopped standby that has replayed past
# the point of divergence, when the last record it replayed is an abort.
#
# This test does not make use of RewindTest as it requires three nodes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1);
$node_a->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = '1h'
));
$node_a->start;

my $backup_name = 'my_backup';
$node_a->backup($backup_name);

my $node_s1 = PostgreSQL::Test::Cluster->new('node_s1');
$node_s1->init_from_backup($node_a, $backup_name, has_streaming => 1);
$node_s1->start;

my $node_s2 = PostgreSQL::Test::Cluster->new('node_s2');
$node_s2->init_from_backup($node_a, $backup_name, has_streaming => 1);
$node_s2->start;

# Restart S2, so that it has no dirty buffers and its minRecoveryPoint is
# at the replay position.
$node_a->wait_for_replay_catchup($node_s2);
$node_s2->restart;

# Give S2 a checkpoint record to create its shutdown restartpoint from, and
# promote S1 right after it.
$node_a->safe_psql('postgres', 'CHECKPOINT');
$node_a->wait_for_replay_catchup($node_s1);
$node_s1->promote;

# Only S2 replays this abort.  Unlike a commit, an abort does not advance
# minRecoveryPoint when it is written out, and it modifies no data page.  It
# is not flushed at once, hence wait for the insert position.
$node_a->safe_psql('postgres',
	'BEGIN; SELECT pg_current_xact_id(); ROLLBACK;');
my $abort_lsn = $node_a->lsn('insert');
$node_a->wait_for_catchup($node_s2, 'replay', $abort_lsn);
$node_s2->stop;

my ($stdout, $stderr) =
  run_command([ 'pg_controldata', $node_s2->data_dir ]);
my ($min_recovery_lsn) =
  $stdout =~ /^Minimum recovery ending location:\s+(\S+)$/m;
is( $node_a->safe_psql(
		'postgres',
		"SELECT '$min_recovery_lsn'::pg_lsn >= '$abort_lsn'::pg_lsn"),
	't',
	'minRecoveryPoint covers the replayed abort after shutdown');

command_checks_all(
	[
		'pg_rewind',
		'--source-server' => $node_s1->connstr('postgres'),
		'--target-pgdata' => $node_s2->data_dir,
	],
	0,
	[],
	[qr/rewinding from last common checkpoint/],
	'pg_rewind rewinds the standby');

done_testing();
