# Copyright (c) 2026, PostgreSQL Global Development Group

# Rolling back index inserts writes compensation records (CLRs) through the
# Undo resource manager that change btree and hash pages.  wal_consistency_
# checking must be able to verify the redo of those page changes, so it has
# to accept "undo", and a standby replaying the CLRs must find every page
# consistent with the primary's image.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"wal_consistency_checking = 'undo,btree,hash'");
$primary->start;

$primary->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE w (id int, val text) WITH (index_undo = on);
CREATE INDEX w_b ON w (id);
CREATE INDEX w_h ON w USING hash (id);
INSERT INTO w SELECT g, 'committed' FROM generate_series(1, 200) g;
});

$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;

$primary->safe_psql(
	'postgres', q{
BEGIN;
SET LOCAL undo_instant_abort_threshold = 2147483647;
INSERT INTO w SELECT g, 'aborted' FROM generate_series(1001, 1300) g;
ROLLBACK;
});
is($primary->safe_psql('postgres', "SELECT idxundo_count_dead_all('w_b')"),
	'300', 'rollback killed the btree entries');
cmp_ok(
	$primary->safe_psql('postgres', "SELECT idxundo_count_dead_all('w_h')"),
	'>', 0, 'rollback killed hash entries');

$primary->wait_for_catchup($standby, 'replay');
my $q = "SET enable_seqscan = off; SET enable_bitmapscan = off; "
  . "SELECT count(*) FROM w WHERE id BETWEEN 1 AND 200";
is($standby->safe_psql('postgres', $q),
	'200', 'standby replayed the CLRs and still finds the committed rows');
ok( !$standby->log_contains('inconsistent page found'),
	'no page inconsistency in the standby log');

$standby->stop;
$primary->stop;
done_testing();
