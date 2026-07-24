
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that cost limit rebalancing reaches parallel autovacuum workers.
#
# Leader pauses at the injection point after the shared cost param snapshot
# (balance = 1, limit 200). A second autovacuum worker starts (balance = 2,
# limit 100). After resume, the parallel workers' first param load must show
# the rebalanced 100, not the snapshotted 200.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

$node->append_conf(
	'postgresql.conf', qq{
autovacuum_naptime = '1s'
autovacuum_max_workers = 3
autovacuum_worker_slots = 4
autovacuum_max_parallel_workers = 2
autovacuum_vacuum_cost_delay = '20ms'
vacuum_cost_limit = 200
max_worker_processes = 16
max_parallel_workers = 8
log_min_messages = debug2
min_parallel_index_scan_size = 0
log_autovacuum_min_duration = -1
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE DATABASE regress_db2');

# leader's table
$node->safe_psql(
	'postgres', qq{
	CREATE TABLE test_av (id serial primary key, col_1 int, col_2 int)
	  WITH (autovacuum_parallel_workers = 2, autovacuum_enabled = false);
	INSERT INTO test_av SELECT g, g + 1, g + 2 FROM generate_series(1, 50000) g;
	CREATE INDEX test_av_col_1 ON test_av (col_1);
	CREATE INDEX test_av_col_2 ON test_av (col_2);
});

# second worker's table: no indexes, no cost reloptions (participates in
# balancing), sized to outlast the test
$node->safe_psql(
	'regress_db2', qq{
	CREATE TABLE filler (id int, pad text) WITH (autovacuum_enabled = false);
	INSERT INTO filler SELECT g, repeat('x', 100) FROM generate_series(1, 200000) g;
});

# quiesce catalogs so no extra worker skews the balance
$node->safe_psql($_, 'VACUUM ANALYZE')
  for ('postgres', 'regress_db2', 'template1');

my $db2oid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'regress_db2'");
my $filleroid = $node->safe_psql('regress_db2', "SELECT 'filler'::regclass::oid");

$node->safe_psql('postgres', 'UPDATE test_av SET col_1 = col_1 + 1');
$node->safe_psql('regress_db2', 'UPDATE filler SET id = id + 1');

my $log_offset = -s $node->logfile;

# pause leader after the shared cost param snapshot
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autovacuum-start-parallel-vacuum', 'wait')");
$node->safe_psql('postgres', 'ALTER TABLE test_av SET (autovacuum_enabled = true)');
$node->wait_for_event('autovacuum worker', 'autovacuum-start-parallel-vacuum');

# second worker -> balance = 2
$node->safe_psql('regress_db2', 'ALTER TABLE filler SET (autovacuum_enabled = true)');
$node->wait_for_log(
	qr/VacuumUpdateCosts\(db=$db2oid, rel=$filleroid, dobalance=yes, cost_limit=100,/,
	$log_offset);

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('autovacuum-start-parallel-vacuum')");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('autovacuum-start-parallel-vacuum')");

# first param load must show the rebalanced limit
$node->wait_for_log(
	qr/parallel autovacuum worker updated cost params: cost_limit=\d+,/,
	$log_offset);
my $log = slurp_file($node->logfile, $log_offset);
my @limits =
  $log =~ /parallel autovacuum worker updated cost params: cost_limit=(\d+),/g;
note("parallel worker cost_limit sequence: @limits");
is($limits[0], '100',
	'parallel workers see the rebalanced cost limit');

my $filler_running = $node->safe_psql('regress_db2',
	"SELECT count(*) FROM pg_stat_progress_vacuum WHERE relid = 'filler'::regclass");
is($filler_running, '1', 'second autovacuum worker was still running');

$node->stop;
done_testing();
