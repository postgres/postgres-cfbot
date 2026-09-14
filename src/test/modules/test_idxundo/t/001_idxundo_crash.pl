# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Crash recovery for index UNDO.
#
# A transaction that inserts into a table WITH (index_undo = on) and is killed
# before it commits must, after recovery, leave no index entry pointing at a row
# that is not in the heap.  The index UNDO records reach the recovery path the
# same way FILEOPS' do: the XLOG_UNDO_BATCH records are replayed, the
# transaction is found to have no commit record, and the chain is applied --
# either during the recovery UNDO phase or, when the syscache is not yet
# available there (always, for index UNDO), deferred to the ATM and drained by
# the logical revert worker for the transaction's database.
#
# A transaction that committed while its index UNDO was still deferred must not
# be mistaken for a loser.
#
# The assertion is deliberately about index/heap AGREEMENT rather than about a
# dead-entry count: whether a given entry ends up LP_DEAD immediately or is left
# for VACUUM is an efficiency question, but an index scan returning a row the
# heap does not have would be corruption.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_logical_revert_workers = 2
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');
my $dboid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'postgres'");

# A table whose indexes write UNDO, plus a committed baseline to protect.
$node->safe_psql(
	'postgres', q{
CREATE TABLE ct (id int primary key, val text) WITH (index_undo = on);
CREATE INDEX ct_val_idx ON ct (val);
INSERT INTO ct SELECT g, 'committed-' || g FROM generate_series(1, 500) g;
});

my $committed = $node->safe_psql('postgres', 'SELECT count(*) FROM ct');
is($committed, '500', 'committed baseline present before crash');

# A committed transaction whose index UNDO is still sitting in the deferred
# batch when it commits: 300 rows x 1 index stays under undo_batch_record_limit,
# so nothing is written until the commit path flushes it.  That batch must reach
# WAL before the commit record; if it follows the commit record, redo removes
# the xid at COMMIT and re-adds it at the batch, and recovery then treats this
# committed transaction as one to roll back.
$node->safe_psql('postgres',
	'CREATE TABLE cc (id int primary key) WITH (index_undo = on)');
my $committed_xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO cc SELECT g FROM generate_series(1, 300) g;
SELECT pg_current_xact_id();
COMMIT;
});

# The same, but the transaction's first index UNDO is written inside a
# subtransaction.  Its batches must still be attributed to the top-level xid:
# the commit record clears only that xid from recovery tracking.
$node->safe_psql('postgres',
	'CREATE TABLE cs (id int primary key) WITH (index_undo = on)');
$node->safe_psql(
	'postgres', q{
BEGIN;
SAVEPOINT s;
INSERT INTO cs SELECT g FROM generate_series(1, 300) g;
RELEASE SAVEPOINT s;
INSERT INTO cs SELECT g FROM generate_series(301, 310) g;
COMMIT;
});

# Start a transaction, insert, and leave it uncommitted.  A background psql
# keeps the session (and so the transaction) open while we kill the server.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe(
	q{INSERT INTO ct SELECT g, 'aborted-' || g FROM generate_series(10001, 10300) g}
);

# The rows are visible to their own transaction, so the insert really happened
# and its UNDO is in WAL.
my $in_txn = $bg->query_safe('SELECT count(*) FROM ct WHERE id >= 10001');
is($in_txn, '300', 'uncommitted rows visible inside their own transaction');
my $killed_xid = $bg->query_safe('SELECT pg_current_xact_id()');

# Index UNDO is deferred and batched; 600 records is below the batch limits, so
# they would still be in backend memory at the crash and never reach WAL (such
# entries are left for VACUUM by design).  Starting a subtransaction is one of
# the points that writes the pending batch, so recovery sees a real loser chain.
$bg->query_safe('SAVEPOINT s');

# Force the WAL out, then crash before the transaction can commit.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');

$node->start;

# The killed transaction never committed, so none of its rows may be visible.
my $after =
  $node->safe_psql('postgres', 'SELECT count(*) FROM ct WHERE id >= 10001');
is($after, '0',
	'rows from the killed transaction are not visible after recovery');

# The committed rows survived.
is($node->safe_psql('postgres', 'SELECT count(*) FROM ct'),
	'500', 'committed rows survived the crash');

# Recovery cannot apply index UNDO itself (no syscache in the startup process),
# so it defers the killed transaction to the ATM.  The entry must carry the
# database the transaction ran in: no revert worker serves any other database,
# so an entry with the wrong one would never drain and would pin WAL forever.
my $log = slurp_file($node->logfile);
like(
	$log,
	qr/UNDO recovery: rolling back transaction $killed_xid\b/,
	'recovery found the killed transaction as a loser');
like(
	$log,
	qr/UNDO recovery: transaction $killed_xid deferred to logical revert worker/,
	'recovery deferred its index UNDO to the logical revert worker');

like(
	$log,
	qr/UNDO recovery: 1 incomplete transaction\(s\) to roll back/,
	'the killed transaction is the only recovery loser');

# The logical revert worker for that database must revert and forget it.
ok( $node->poll_query_until(
		'postgres', "SELECT count(*) = 0 FROM idxundo_atm_entries()"),
	'logical revert worker drained the deferred ATM entry');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE dboid <> $dboid"),
	'0',
	'no ATM entry is left for a database without a revert worker');

# The worker leaves the killed transaction's index entries for VACUUM: after
# the abort is visible their heap TIDs could hold committed rows, so index UNDO
# is applied only by the aborting backend itself.
is( $node->safe_psql('postgres', "SELECT idxundo_count_dead_all('ct_pkey')"),
	'0',
	'killed transaction\'s primary key entries are left for VACUUM');
is( $node->safe_psql(
		'postgres', "SELECT idxundo_count_dead_all('ct_val_idx')"),
	'0',
	'killed transaction\'s secondary index entries are left for VACUUM');

# The property that matters: every index agrees with the heap.  A forced
# index-only path must not produce a row the heap scan does not, and amcheck
# must find no entry without a matching heap tuple.
for my $idx ('ct_pkey', 'ct_val_idx')
{
	my ($rc, $stdout, $stderr) = $node->psql('postgres',
		"SELECT bt_index_check('$idx', heapallindexed => true)");
	is($rc, 0, "amcheck clean on $idx after crash recovery");
	is($stderr, '', "amcheck reported nothing on $idx");
}

my $viaidx = $node->safe_psql(
	'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM ct WHERE id BETWEEN 1 AND 500;
});
is($viaidx, '500', 'all committed rows still reachable through the index');

my $ghosts = $node->safe_psql(
	'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM ct WHERE id BETWEEN 10001 AND 10300;
});
is($ghosts, '0', 'no index entry resolves to a killed transaction row');

# The same check for the secondary index, which is scanned by value.
my $ghosts_val = $node->safe_psql(
	'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM ct WHERE val LIKE 'aborted-%';
});
is($ghosts_val, '0', 'no secondary-index entry resolves to a killed row');

# The committed transaction is not a recovery loser, and its index entries are
# all still live.
unlike(
	$log,
	qr/UNDO recovery: rolling back transaction $committed_xid\b/,
	'committed transaction with deferred index UNDO is not rolled back');
is( $node->safe_psql(
		'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM cc WHERE id BETWEEN 1 AND 300;
}),
	'300',
	'committed rows with deferred index UNDO still reachable through the index'
);
is( $node->safe_psql('postgres', "SELECT idxundo_count_dead_all('cc_pkey')"),
	'0',
	'no index entry of the committed transaction was marked dead');
is( $node->safe_psql('postgres', "SELECT idxundo_count_dead_all('cs_pkey')"),
	'0',
	'no index entry of the committed subtransaction-first writer was marked dead'
);
is( $node->safe_psql(
		'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM cs WHERE id BETWEEN 1 AND 310;
}),
	'310',
	'committed rows first written in a subtransaction reachable through the index'
);

$node->stop;
done_testing();
