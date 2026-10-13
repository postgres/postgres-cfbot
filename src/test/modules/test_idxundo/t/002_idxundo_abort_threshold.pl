# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# undo_instant_abort_threshold: an abort whose UNDO is at least this many bytes
# is handed to the logical revert worker through the ATM; a smaller one is
# applied inline by the aborting backend.  The revert launcher is disabled so a
# deferred abort stays observable in the ATM.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_threshold');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 0
undo_instant_abort_threshold = 65536
});
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key) WITH (index_undo = on);
CREATE TABLE big (id int primary key) WITH (index_undo = on);
});

# Small abort: a handful of index entries, far below the threshold.
my $small_xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 10) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
is( $node->safe_psql(
		'postgres', 'SELECT count(*) FROM idxundo_atm_entries()'),
	'0',
	'small abort is applied inline, not queued in the ATM');
is($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('t_pkey')"),
	'10', 'small abort marked its index entries dead inline');

# Large abort: 5000 index UNDO records, several flushed batches and well over
# 64kB of UNDO in total.
my $big_xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO big SELECT g FROM generate_series(1, 5000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$big_xid'"),
	'1',
	'abort above undo_instant_abort_threshold is queued in the ATM');
is( $node->safe_psql('postgres', "SELECT idxundo_count_dead_all('big_pkey')"),
	'0',
	'large abort was not applied inline');

# The retention view reports what holds UNDO WAL back: the queued abort, and an
# in-flight transaction that has written UNDO.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_get_undo_retention() WHERE holder = 'aborted' AND xid = '$big_xid' AND oldest_lsn IS NOT NULL AND retained_bytes > 0"
	),
	'1',
	'pg_stat_get_undo_retention reports the queued abort');
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('INSERT INTO t VALUES (100001)');
$bg->query_safe('SAVEPOINT s');    # writes the deferred index UNDO batch
my $pid = $bg->query_safe('SELECT pg_backend_pid()');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_get_undo_retention() WHERE holder = 'backend' AND pid = $pid"
	),
	'1',
	'pg_stat_get_undo_retention reports an in-flight transaction');
$bg->query_safe('ROLLBACK');
$bg->quit;

$node->stop;
done_testing();
