# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# ROLLBACK PREPARED hands a large prepared transaction's index UNDO to the
# logical revert worker of the database the transaction was PREPARED in.
#
# Finishing a prepared transaction from another database is refused, so the
# database must come from the 2PC state, not the finishing session -- which
# also covers a transaction recovered from the 2PC state after a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_2pc_db');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_prepared_transactions = 2
max_logical_revert_workers = 0
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE db1');
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');
$node->safe_psql(
	'db1', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key) WITH (index_undo = on);
});
my $db1 = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'db1'");

$node->safe_psql(
	'db1', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 5000) g;
PREPARE TRANSACTION 'p1';
});

my ($rc, $out, $err) = $node->psql('postgres', "ROLLBACK PREPARED 'p1'");
like(
	$err,
	qr/prepared transaction belongs to another database/,
	'ROLLBACK PREPARED from another database is refused');

# Crash, so the transaction is recovered from its 2PC state, then roll it
# back.  The ATM entry must name db1.
$node->stop('immediate');
$node->start;
$node->safe_psql('db1', "ROLLBACK PREPARED 'p1'");
is( $node->safe_psql('postgres', 'SELECT dboid FROM idxundo_atm_entries()'),
	$db1,
	'the rolled-back prepared transaction is queued for its own database');

# The revert worker for db1 reverts it.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'the revert worker for that database drained it');
# The worker leaves index entries for VACUUM (see nbtree_undo_apply()).
is($node->safe_psql('db1', "SELECT idxundo_count_dead_all('t_pkey')"),
	'0', 'the prepared transaction\'s index entries are left for VACUUM');

$node->stop;
done_testing();
