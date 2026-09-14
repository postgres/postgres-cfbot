# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# A FILEOPS UNDO record that cannot be applied must be reported as a failure,
# so the abort hands the chain to the logical revert worker instead of
# treating it as rolled back.
#
# The transaction truncates a file, which is then replaced behind its back by a
# directory of the same name, so the truncation's UNDO cannot reopen it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_apply_failure');
$node->init;
# FILEOPS writes UNDO only when WAL is needed.
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $path = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('fileops_fail.dat')");

my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_truncate('$path', 10)");
unlink($path) or die "unlink $path: $!";
mkdir($path) or die "mkdir $path: $!";
$bg->query('ROLLBACK');
$bg->quit;

like(
	slurp_file($node->logfile),
	qr/FILEOPS UNDO TRUNCATE: could not open/,
	'the failure was reported');

$node->stop;
done_testing();
