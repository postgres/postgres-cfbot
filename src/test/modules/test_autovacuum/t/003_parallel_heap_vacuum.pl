
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test parallel heap vacuum by autovacuum
#
# TODO:
# - a scan resumed after the dead TID store fills up
# - eager scanning, with the caps shared between the processes
# - the wraparound failsafe reaching the workers
# - a table with no indexes, and more such cases

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

$node->append_conf(
	'postgresql.conf', qq{
autovacuum_max_workers = 1
autovacuum_worker_slots = 2
autovacuum_max_parallel_workers = 2
max_worker_processes = 10
max_parallel_workers = 10
autovacuum_naptime = '1s'
min_parallel_table_scan_size = 0
autovacuum_vacuum_threshold = 100000
autovacuum_analyze_threshold = 100000
autovacuum_vacuum_insert_threshold = -1
log_autovacuum_min_duration = -1
});
$node->start;

# One row per page, enough pages for more than one chunk per process.
$node->safe_psql(
	'postgres', qq{
	CREATE TABLE t (id int, filler text)
		WITH (autovacuum_enabled = off, fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 700) FROM generate_series(1, 6200) g;
	CREATE INDEX ON t (id);
	DELETE FROM t WHERE id % 2 = 1;
});

my $log_offset = -s $node->logfile;

$node->safe_psql(
	'postgres', qq{
	ALTER TABLE t SET (autovacuum_parallel_workers = 2,
					   autovacuum_vacuum_threshold = 50,
					   autovacuum_vacuum_scale_factor = 0,
					   log_autovacuum_min_duration = 0,
					   autovacuum_enabled = true);
});

$node->wait_for_log(qr/parallel workers: heap scan:/, $log_offset);

like(
	slurp_file($node->logfile, $log_offset),
	qr/tuples: [1-9]\d* removed.*parallel workers: heap scan: 2 planned, 2 launched in total/s,
	'dead rows removed with two parallel heap scan workers');

$node->stop;
done_testing();
