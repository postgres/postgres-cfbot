# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# An ATM entry the logical revert worker cannot finish must not hold up the
# other entries of its database, and a malformed UNDO chain, which can never
# be walked, must be given up on rather than retried (and its WAL pinned)
# forever.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_liveness');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
});
$node->start;
# No autovacuum: it would remove the aborted entries before the worker does.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

# The oldest entry has a malformed chain: it points at a checkpoint record.
# Its xid wrote nothing, so treating it as aborted changes no data.
my $bad = $node->safe_psql('postgres',
	'BEGIN; SELECT pg_current_xact_id(); COMMIT;');
($bad) = $bad =~ /(\d+)\s*$/;
$node->safe_psql('postgres', 'CHECKPOINT');
is( $node->safe_psql(
		'postgres',
		"SELECT idxundo_atm_add('$bad', checkpoint_lsn) FROM pg_control_checkpoint()"
	),
	't',
	'malformed entry queued');

# A newer, good entry: a large abort the worker has to revert.
my $good = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 5000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
($good) = $good =~ /(\d+)\s*$/;
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$good'"),
	'1',
	'large abort queued');

my $log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;

ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$good'"),
	'the entry behind the malformed one is reverted');
cmp_ok($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('t_pkey')"),
	'>=', 4900, 'its whole chain was reverted');

$node->wait_for_log(
	qr/giving up on transaction $bad, whose UNDO chain is malformed/,
	$log_offset);
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'the malformed entry is forgotten');

$node->stop;
done_testing();
