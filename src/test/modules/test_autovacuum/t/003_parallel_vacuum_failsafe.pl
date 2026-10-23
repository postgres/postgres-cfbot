
# Copyright (c) 2026, PostgreSQL Global Development Group

# Parallel vacuum checks the wraparound failsafe after each index, and skips
# the indexes it has not started once the failsafe has triggered.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Only the test table meets its thresholds and one worker runs at a time, so
# no other table's worker gets held at the point. The failsafe age is never
# less than autovacuum_freeze_max_age * 1.05, so 105000 XIDs here.
$node->append_conf(
	'postgresql.conf', qq{
autovacuum_naptime = '1s'
autovacuum_max_workers = 1
autovacuum_max_parallel_workers = 1
autovacuum_vacuum_threshold = 100000
autovacuum_vacuum_insert_threshold = -1
autovacuum_freeze_max_age = 100000
vacuum_failsafe_age = 100000
min_parallel_index_scan_size = 0
});
$node->start;

# The modules may be absent under installcheck.
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('xid_wraparound'))
{
	plan skip_all => 'Required test extensions are not installed';
}

# The leader and its one parallel worker take one index each of the four,
# then wait at the injection point.
$node->safe_psql(
	'postgres', qq{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION xid_wraparound;
	CREATE TABLE test_failsafe (col int)
		WITH (autovacuum_enabled = false,
			  autovacuum_parallel_workers = 1,
			  autovacuum_vacuum_threshold = 1,
			  autovacuum_vacuum_scale_factor = 0,
			  log_autovacuum_min_duration = 0);
	INSERT INTO test_failsafe SELECT generate_series(1, 100);
	CREATE INDEX idx_1 ON test_failsafe (col);
	CREATE INDEX idx_2 ON test_failsafe (col);
	CREATE INDEX idx_3 ON test_failsafe (col);
	CREATE INDEX idx_4 ON test_failsafe (col);
	DELETE FROM test_failsafe;
	SELECT injection_points_attach('parallel-vacuum-failsafe-check', 'wait');
});

my $log_offset = -s $node->logfile;
$node->safe_psql('postgres',
	'ALTER TABLE test_failsafe SET (autovacuum_enabled = true)');
$node->wait_for_event('autovacuum worker', 'parallel-vacuum-failsafe-check');
$node->wait_for_event('parallel worker', 'parallel-vacuum-failsafe-check');

$node->safe_psql('postgres', 'SELECT consume_xids(150000)');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('parallel-vacuum-failsafe-check')");

# Release the two processes, a wakeup reaches only the first one registered
foreach my $left (1, 0)
{
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('parallel-vacuum-failsafe-check')");
	$node->poll_query_until('postgres',
		"SELECT count(*) = $left FROM pg_stat_activity
		 WHERE wait_event = 'parallel-vacuum-failsafe-check'")
	  or die "timed out releasing a held vacuum process";
}

# The report lists only the indexes that were vacuumed
$node->wait_for_log(
	qr/automatic vacuum of table "postgres\.public\.test_failsafe"/,
	$log_offset);
my @vacuumed =
  slurp_file($node->logfile, $log_offset) =~ /index "(idx_\d)":/g;
note("indexes vacuumed: @vacuumed");

is(scalar(@vacuumed), 2,
	'remaining indexes are skipped once the failsafe triggers');

$node->stop;
done_testing();
