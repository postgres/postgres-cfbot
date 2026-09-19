# Copyright (c) 2026, PostgreSQL Global Development Group

# The logical revert launcher starts a per-database revert worker for every
# database that has unreverted ATM entries and tracks the databases it has
# started a worker for, so it does not start a second one.  Starting a worker
# can fail: RegisterDynamicBackgroundWorker() returns false when the global
# background-worker slots are exhausted (parallel query, logical replication,
# other workers).  StartLogicalRevertWorker() was void and only WARNed on that
# failure, and the launcher appended the database to its tracked set
# unconditionally.  A database whose worker failed to start was therefore
# recorded as "has a worker" and skipped on every later scan: no worker ever
# started for it, its aborted-transaction UNDO was never applied, and
# ATMGetOldestUnrevertedLSN pinned that WAL forever.
#
# The injection point logical-revert-register-fail makes StartLogicalRevertWorker()
# take its registration-failure path without actually exhausting the slots.
# The launcher normally scans once a minute; SIGHUP (reload) wakes it for an
# immediate scan.  While the injection point is attached the worker cannot
# start; once it is detached, a correct launcher retries the database on its
# next scan and the entry is reverted.  Before the fix the launcher has marked
# the database tracked and never retries it, so the entry stays unreverted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_launcher_register_fail');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 2
autovacuum = off
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

# Make every attempt to start a revert worker fail, as if no bgworker slot were
# free, before any ATM entry exists for this database.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('logical-revert-register-fail', 'notice')");

# A large abort queues an ATM entry.  Wake the launcher (SIGHUP) so it scans
# now instead of at its next once-a-minute wake-up, and tries -- and fails --
# to start a worker for this database.
my $log_offset = -s $node->logfile;
my $xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 5000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
($xid) = $xid =~ /(\d+)\s*$/;
$node->reload;

$node->wait_for_log(
	qr/could not register logical revert worker for database/, $log_offset);
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$xid'"),
	'1',
	'entry is queued but no worker could start');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'logical revert worker'"
	),
	'0',
	'no revert worker is running');

# Let worker registration succeed again, and wake the launcher for another
# scan.  A launcher that did not wrongly mark the database tracked retries it
# and reverts the entry; the buggy one has it tracked and never retries.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('logical-revert-register-fail')");
$node->reload;

my $reverted = $node->poll_query_until(
	'postgres',
	"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$xid'",
	't');
ok($reverted,
	'the stranded database is retried once a worker can start, and reverted');

$node->stop;
done_testing();
