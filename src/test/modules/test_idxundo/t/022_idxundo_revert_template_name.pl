
# Copyright (c) 2026, PostgreSQL Global Development Group

# The logical revert launcher skips template databases when it looks for
# databases whose ATM entries need a worker.  It recognised them by name, as
# any database whose name begins with "template", so a user database named,
# say, "templates" never got a worker: its aborted transactions stayed
# unreverted and their WAL was kept for good.  It now goes by datistemplate.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_template_name');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 2
logical_revert_naptime = 100ms
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE templates');
$node->safe_psql(
	'templates', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

# A large abort hands its index UNDO to the ATM for the revert worker.
my $xid = $node->safe_psql(
	'templates', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 5000) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
($xid) = $xid =~ /(\d+)\s*$/;
$node->reload;    # wake the launcher

ok( $node->poll_query_until(
		'templates',
		"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$xid'"),
	'an aborted transaction in a database named "templates" is reverted');

$node->stop;
done_testing();
