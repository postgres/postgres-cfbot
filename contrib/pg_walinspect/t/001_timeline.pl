# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_get_wal_files() follows the current timeline's history across
# a promotion.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub test_timeline_history
{
	my ($suffix, $archive_mode) = @_;
	my $primary = PostgreSQL::Test::Cluster->new("primary_$suffix");
	my $standby = PostgreSQL::Test::Cluster->new("standby_$suffix");

	$primary->init(allows_streaming => 1);
	$primary->append_conf('postgresql.conf', "autovacuum = off\n");
	if ($archive_mode)
	{
		$primary->append_conf(
			'postgresql.conf',
			"archive_mode = on\narchive_command = 'true'\n");
	}
	$primary->start;
	$primary->safe_psql('postgres',
		'CREATE EXTENSION pg_walinspect; CREATE TABLE test_table (a int);');
	$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
	my $boundary_lsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
	my $boundary_offset = $primary->safe_psql('postgres',
		"SELECT file_offset FROM pg_walfile_name_offset('$boundary_lsn')");
	is($boundary_offset, '0',
		"WAL is at a segment boundary with archive_mode=$archive_mode");
	my $boundary_files = $primary->safe_psql('postgres',
		"SELECT count(*) FROM pg_get_wal_files('$boundary_lsn')");
	is($boundary_files, '1',
		"point lookup works at a segment boundary with archive_mode=$archive_mode");

	my $backup_name = "backup_$suffix";
	$primary->backup($backup_name);
	$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
	$standby->append_conf('postgresql.conf', "wal_keep_size = '64MB'\n");
	$standby->start;

	$primary->safe_psql('postgres', 'INSERT INTO test_table VALUES (1)');
	my $start_lsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
	$primary->safe_psql('postgres', 'INSERT INTO test_table VALUES (2)');
	$primary->wait_for_catchup($standby);

	$standby->promote;
	$standby->safe_psql('postgres', 'INSERT INTO test_table VALUES (3)');

	my $result = $standby->safe_psql(
		'postgres', qq{
SELECT count(DISTINCT left(wal_file, 8)) = 2,
       min(left(wal_file, 8)) = '00000001',
       max(left(wal_file, 8)) = '00000002'
FROM pg_get_wal_files('$start_lsn', pg_current_wal_lsn())});
	is($result, 't|t|t',
		"WAL files span the timeline switch with archive_mode=$archive_mode");

	$standby->stop;
	$primary->stop;
	return;
}

test_timeline_history('archive_off', 0);
test_timeline_history('archive_on', 1);

done_testing();
