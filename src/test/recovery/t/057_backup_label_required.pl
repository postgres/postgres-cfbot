# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test the pg_control flag that makes backup_label mandatory for recovery.
#
# pg_basebackup stores a modified copy of pg_control in the backup, with a flag
# set that makes recovery refuse to start if backup_label is missing.  This
# prevents the silent corruption that results from removing the file, both for
# backups taken from a primary and from a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Rename backup_label in the data directory of the given node, start it, and
# check that recovery refuses to proceed.  Then put the file back.
sub check_startup_without_backup_label
{
	my ($node, $test_name) = @_;
	my $data_dir = $node->data_dir;
	my $log_offset = -s $node->logfile;

	rename("$data_dir/backup_label", "$data_dir/backup_label.tmp")
	  or BAIL_OUT("could not rename $data_dir/backup_label");

	is($node->start(fail_ok => 1), 0, "$test_name: startup fails");
	ok( $node->log_contains(
			'FATAL: .*could not find backup_label required for recovery',
			$log_offset),
		"$test_name: ends with FATAL for missing backup_label");

	rename("$data_dir/backup_label.tmp", "$data_dir/backup_label")
	  or BAIL_OUT("could not rename $data_dir/backup_label.tmp");
	return;
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql('postgres',
	'CREATE TABLE tab_int AS SELECT generate_series(1, 1000) AS a');

# Take a backup from the primary.  The copy of pg_control stored in the backup
# must require backup_label, while the control file of the running cluster is
# left alone.
my $backup_name = 'backup_primary';
$node_primary->backup($backup_name);

command_like(
	[
		'pg_controldata',
		'--pgdata' => $node_primary->backup_dir . '/' . $backup_name
	],
	qr/Backup label required: +yes/,
	'backup taken from a primary requires backup_label');
command_like(
	[ 'pg_controldata', '--pgdata' => $node_primary->data_dir ],
	qr/Backup label required: +no/,
	'control file of the source cluster is unchanged');
is( $node_primary->safe_psql(
		'postgres', 'SELECT backup_label_required FROM pg_control_recovery()'),
	'f',
	'pg_control_recovery() reports the flag');

# Restoring that backup without backup_label must not start.
my $node_restored = PostgreSQL::Test::Cluster->new('restored');
$node_restored->init_from_backup($node_primary, $backup_name);

check_startup_without_backup_label($node_restored, 'backup from primary');

# With backup_label back in place recovery completes, and the flag is cleared
# so that subsequent restarts no longer need the file.
$node_restored->start;
is($node_restored->safe_psql('postgres', 'SELECT count(*) FROM tab_int'),
	1000, 'restored cluster has the expected contents');
is( $node_restored->safe_psql(
		'postgres', 'SELECT backup_label_required FROM pg_control_recovery()'),
	'f',
	'flag is cleared once recovery has completed');
$node_restored->stop;

command_like(
	[ 'pg_controldata', '--pgdata' => $node_restored->data_dir ],
	qr/Backup label required: +no/,
	'control file no longer requires backup_label after recovery');

# A backup taken from a standby gets the same treatment.  This is the case that
# previously required backup software to copy pg_control last.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;
$node_primary->wait_for_replay_catchup($node_standby);

my $standby_backup = 'backup_standby';
$node_standby->backup($standby_backup);

command_like(
	[
		'pg_controldata',
		'--pgdata' => $node_standby->backup_dir . '/' . $standby_backup
	],
	qr/Backup label required: +yes/,
	'backup taken from a standby requires backup_label');

my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->init_from_backup($node_standby, $standby_backup,
	has_streaming => 1);

check_startup_without_backup_label($node_standby2, 'backup from standby');

$node_standby2->start;
$node_standby->wait_for_replay_catchup($node_standby2, $node_primary);
is($node_standby2->safe_psql('postgres', 'SELECT count(*) FROM tab_int'),
	1000, 'cascading standby from a standby backup is caught up');

done_testing();
