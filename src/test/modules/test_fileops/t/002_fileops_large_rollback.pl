# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# A transaction whose FILEOPS UNDO is larger than undo_instant_abort_threshold
# must still be rolled back by its own ROLLBACK, not handed to the logical
# revert worker: filesystem effects have to be gone when ROLLBACK returns (and
# with no revert worker they would otherwise never go away).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_large_rollback');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_logical_revert_workers = 0
undo_instant_abort_threshold = 1024
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
my $dir = $node->data_dir;

# 50 creates are several kB of FILEOPS UNDO, well over the threshold.
$node->safe_psql(
	'postgres', qq{
BEGIN;
SELECT test_fileops_create('$dir/large_rb_' || g, 384)
  FROM generate_series(1, 50) g;
ROLLBACK;
});

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM generate_series(1, 50) g WHERE test_fileops_file_exists('$dir/large_rb_' || g)"
	),
	'0',
	'ROLLBACK removed every file the transaction created');
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	't',
	'nothing was left for the logical revert worker');

$node->stop;
done_testing();
