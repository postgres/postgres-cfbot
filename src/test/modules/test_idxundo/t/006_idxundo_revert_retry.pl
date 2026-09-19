# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# The logical revert worker must not forget an ATM entry whose UNDO chain it
# could not read in full.  A read failure that goes away (simulated with an
# injection point) leaves the entry queued, and the next pass reverts it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_retry');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 2
logical_revert_naptime = 100ms
});
$node->start;
# No autovacuum: it would remove the aborted entries before the worker does.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_idxundo;
CREATE TABLE warm (id int primary key) WITH (index_undo = on);
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

# Get a revert worker running for this database: the launcher starts one at
# startup for a database with ATM entries (otherwise only once a minute), and
# the worker then stays.
$node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO warm SELECT g FROM generate_series(1, 5000) g;
ROLLBACK;
});
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'revert worker is running');

$node->safe_psql('postgres',
	"SELECT injection_points_attach('undo-read-batch-fail', 'notice')");

# A large abort goes to the ATM; the injection point makes every read of its chain fail.
my $xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 5000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
($xid) = $xid =~ /(\d+)\s*$/;

$node->wait_for_log(
	qr/could not read the whole UNDO chain of transaction $xid, will retry/);
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$xid'"),
	'1',
	'entry whose chain could not be read stays in the ATM');

# The failure clears; the worker retries and reverts the whole chain.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('undo-read-batch-fail')");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$xid'"),
	'entry is reverted and forgotten on retry');
# The worker leaves index entries for VACUUM (see nbtree_undo_apply()).
is($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('t_pkey')"),
	'0', 'its index entries are left for VACUUM');

# ERROR must abort, not commit a transaction with outstanding resources.
$node->safe_psql(
	'postgres', q{
SELECT injection_points_attach('logical-revert-process-error',
    'test_idxundo', 'idxundo_error_with_resources', ''::bytea);
});
my $worker_pid = $node->safe_psql(
	'postgres', q{
SELECT pid FROM pg_stat_activity WHERE backend_type = 'logical revert worker';
});
my $error_xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(10001, 15000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
$node->wait_for_log(
	qr/test_idxundo: error with resources at logical-revert-process-error/);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('logical-revert-process-error')");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$error_xid'"
	),
	'worker retries entry after ERROR');
is( $node->safe_psql(
		'postgres', q{
SELECT pid FROM pg_stat_activity WHERE backend_type = 'logical revert worker';
}),
	$worker_pid,
	'same worker survives ERROR');
$node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(20001, 25000) g;
ROLLBACK;
});
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'next entry is also processed');
unlike(
	slurp_file($node->logfile),
	qr/Assertion|WARNING:.*(?:buffer.*(?:pin|leak)|relcache reference leak)|TRAP:/,
	'no assertion or resource leak');

$node->stop;
done_testing();
