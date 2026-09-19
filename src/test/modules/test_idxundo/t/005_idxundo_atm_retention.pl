# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# The WAL holding a deferred rollback's UNDO must survive until the revert
# worker has applied it -- all of it, not only the newest batch.
#
# An abort handed to the ATM, and a prepared transaction, must pin WAL from the
# OLDEST batch of their chain.  With 1MB WAL segments a large transaction's
# chain spans many segments; checkpoints taken before the revert worker runs
# must not recycle any of them, or the worker reverts only part of the chain.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_retention');
$node->init(extra => ['--wal-segsize=1']);
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 0
max_prepared_transactions = 2
min_wal_size = 2MB
max_wal_size = 4MB
checkpoint_timeout = 1h
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE a (id int primary key) WITH (index_undo = on);
CREATE TABLE p (id int primary key) WITH (index_undo = on);
});

my $nrows = 40000;

# Recycle every WAL segment nothing pins.
sub recycle_wal
{
	for (1 .. 3)
	{
		$node->safe_psql('postgres',
			'SELECT pg_switch_wal(); CREATE TABLE junk AS SELECT g FROM generate_series(1, 20000) g; DROP TABLE junk; CHECKPOINT;'
		);
	}
}

# Run the revert worker (it is off until now) and check that it walked the
# whole chain of $table's index: it forgets an entry only after reading every
# batch, and a removed batch is a PANIC.  Each phase has a single deferred
# chain, so no other entry's pin can cover for a missing one.
sub revert_and_check
{
	my ($table, $what) = @_;

	$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
	$node->restart;
	ok( $node->poll_query_until(
			'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
		"revert worker drained the ATM ($what)");

	# The worker leaves index entries for VACUUM (see nbtree_undo_apply()).
	is( $node->safe_psql('postgres',
			"SELECT idxundo_count_dead_all('${table}_pkey')"),
		'0',
		"the $what chain's index entries are left for VACUUM");

	$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
	$node->restart;
}

# A large abort goes to the ATM, and only the ATM entry pins its WAL.
$node->safe_psql(
	'postgres', qq{
BEGIN;
INSERT INTO a SELECT g FROM generate_series(1, $nrows) g;
ROLLBACK;
});
is($node->safe_psql('postgres', 'SELECT count(*) FROM idxundo_atm_entries()'),
	'1', 'large abort is queued in the ATM');
recycle_wal();
revert_and_check('a', 'aborted');

# A large prepared transaction: only the 2PC state pins its WAL, until ROLLBACK
# PREPARED hands it to the ATM.
$node->safe_psql(
	'postgres', qq{
BEGIN;
INSERT INTO p SELECT g FROM generate_series(1, $nrows) g;
PREPARE TRANSACTION 'big';
});
recycle_wal();
$node->safe_psql('postgres', "ROLLBACK PREPARED 'big'");
recycle_wal();
revert_and_check('p', 'rolled-back prepared');

unlike(slurp_file($node->logfile),
	qr/could not read batch|no longer exists \(recycled/,
	'no UNDO batch was recycled before it was applied');

$node->stop;
done_testing();
