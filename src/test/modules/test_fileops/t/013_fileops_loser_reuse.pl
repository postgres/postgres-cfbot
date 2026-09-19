# Copyright (c) 2026, PostgreSQL Global Development Group

# Startup can revert FILEOPS without the syscache needed for index UNDO.
# A deferred worker must not repeat those inverses after same-path reuse.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $crash_again (0, 1)
{
	my $node = PostgreSQL::Test::Cluster->new("loser_$crash_again");
	$node->init;
	$node->append_conf(
		'postgresql.conf', q{
wal_level = replica
autovacuum = off
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
});
	$node->start;
	$node->safe_psql(
		'postgres', q{
CREATE EXTENSION test_fileops;
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key) WITH (index_undo = on);
CHECKPOINT;
});
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('BEGIN');
	$bg->query_safe('INSERT INTO t SELECT generate_series(1, 10)');
	$bg->query_safe("SELECT test_fileops_create('loser.dat', 384)");
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
	$node->stop('immediate');
	$bg->{run}->finish;
	$node->start;
	my $path = $node->data_dir . '/loser.dat';
	ok(!-e $path, "$crash_again: startup removed loser file");
	is( $node->safe_psql(
			'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
		't',
		"$crash_again: index chain deferred");
	$node->safe_psql(
		'postgres', q{
BEGIN;
SELECT test_fileops_create('loser.dat', 384);
SELECT test_fileops_write('loser.dat', 0, 'T2-committed');
COMMIT;
});
	if ($crash_again)
	{
		# No user checkpoint: recover the end-of-recovery ATM snapshot and
		# its subsequent WAL while the worker is still disabled.
		$node->stop('immediate');
		$node->start;
		is(-e $path ? slurp_file($path) : '<missing>',
			'T2-committed',
			'second crash preserves committed reuse before worker runs');
		is( $node->safe_psql(
				'postgres',
				'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
			't',
			'second crash retains deferred index chain');
	}
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
	$node->restart;
	ok( $node->poll_query_until(
			'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
		"$crash_again: worker drained ATM");
	# The worker leaves index entries for VACUUM (see nbtree_undo_apply()).
	is( $node->safe_psql(
			'postgres', "SELECT idxundo_count_dead_all('t_pkey')"),
		'0',
		"$crash_again: worker left index entries for VACUUM");
	is(-e $path ? slurp_file($path) : '<missing>',
		'T2-committed',
		"$crash_again: committed reuse survives deferred loser revert");
	$node->stop;
}

done_testing();
