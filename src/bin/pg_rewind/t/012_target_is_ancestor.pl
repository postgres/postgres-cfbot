# Copyright (c) 2021-2026, PostgreSQL Global Development Group

#
# Test cases where the target is a direct ancestor of the source.
#
# We set up two timelines like this:
#
#            -------------- TLI 2 (source)
#           /
#  ---A----B-------C------- TLI 1
#
#
# On TLI 1, we take a backup at points A, B and C. We use a server
# restored from each of those points as the target, and a primary
# server running on TLI 2 as the source.
#
# - A is a direct ancestor of the source, so no rewind is required.
#   Target WAL ends before the calculated divergence LSN; that used
#   to trip Assert(target_wal_endrec == divergerec).
# - B is also a direct ancestor, at the divergence point. No rewind.
# - C is not a direct ancestor, so rewind is required.
#

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Copy;

my $tmp_folder = PostgreSQL::Test::Utils::tempdir;

my $orig_node = PostgreSQL::Test::Cluster->new('orig_node');
$orig_node->init(allows_streaming => 1);
$orig_node->append_conf(
	'postgresql.conf', qq(
wal_log_hints = on
));
$orig_node->start;
$orig_node->safe_psql('postgres', 'CREATE TABLE test_tab (t TEXT)');
$orig_node->safe_psql('postgres',
	"INSERT INTO test_tab VALUES ('at the beginning')");
$orig_node->stop;
$orig_node->backup_fs_cold('backup_A');

$orig_node->start;
$orig_node->safe_psql('postgres',
	"INSERT INTO test_tab VALUES ('after backup A')");
$orig_node->stop;
$orig_node->backup_fs_cold('backup_B');

# Restore backup B to create Timeline 2
my $node_2 = PostgreSQL::Test::Cluster->new('node_2');
$node_2->init_from_backup($orig_node, 'backup_B', has_streaming => 1);
$node_2->start;
$node_2->promote;
$node_2->safe_psql('postgres', "INSERT INTO test_tab VALUES ('in node_2')");
$node_2->stop;

$orig_node->start;
$orig_node->safe_psql('postgres',
	"INSERT INTO test_tab VALUES ('this will be lost on rewind')");
$orig_node->stop;
$orig_node->backup_fs_cold('backup_C');

my $source_node = $node_2;
my $source_pgdata = $source_node->data_dir;

sub rewind_from_backup
{
	my ($backup, $expected_stderr) = @_;

	my $target_node = PostgreSQL::Test::Cluster->new('restored_' . $backup);
	$target_node->init_from_backup($orig_node, $backup, has_streaming => 1);
	my $target_pgdata = $target_node->data_dir;

	# Keep a temporary postgresql.conf or it would be overwritten during the rewind.
	copy("$target_pgdata/postgresql.conf",
		"$tmp_folder/postgresql.conf.tmp");

	command_checks_all(
		[
			'pg_rewind',
			'--debug',
			'--no-sync',
			'--source-pgdata' => $source_pgdata,
			'--target-pgdata' => $target_pgdata,
		],
		0,
		[],
		[$expected_stderr],
		"pg_rewind on node restored from $backup");

	# Now move back postgresql.conf with old settings
	move("$tmp_folder/postgresql.conf.tmp",
		"$target_pgdata/postgresql.conf");

	my $node_2_connstr = $node_2->connstr();
	$target_node->append_conf(
		'postgresql.conf', qq(
primary_conninfo = '$node_2_connstr'
));
	$target_node->set_standby_mode();
	$target_node->start;

	$node_2->start;
	$node_2->wait_for_catchup($target_node->name);
	my $result =
	  $target_node->safe_psql('postgres', "SELECT * FROM test_tab");
	is( $result, qq(at the beginning
after backup A
in node_2),
		"check query after rewind from $backup");
	$target_node->stop;

	$node_2->stop;
}

rewind_from_backup('backup_C',
	qr/pg_rewind: rewinding from last common checkpoint at .* on timeline 1/);
rewind_from_backup('backup_B', qr/pg_rewind: no rewind required/);
rewind_from_backup('backup_A', qr/pg_rewind: no rewind required/);

done_testing();
