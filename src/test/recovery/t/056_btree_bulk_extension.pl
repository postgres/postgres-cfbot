# Copyright (c) 2026, PostgreSQL Global Development Group

# Test recovery when B-tree bulk extension makes the primary's main fork
# longer than the standby's.  Relation extension is not WAL-logged, while an
# index FSM page can reach the standby in a hint full-page image.  The standby
# may therefore have FSM entries for reserved pages that do not exist in its
# main fork.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, data_checksums => 1);
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
shared_buffers = '16MB'
});
$primary->start;

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

$primary->safe_psql(
	'postgres', q{
CREATE TABLE btree_bulk_extension_test (i integer);
CREATE INDEX btree_bulk_extension_idx ON btree_bulk_extension_test (i);
INSERT INTO btree_bulk_extension_test SELECT i FROM generate_series(1, 10000) i;
});

$primary->wait_for_replay_catchup($standby);

my $primary_pages = $primary->safe_psql(
	'postgres',
	q{SELECT pg_relation_size('btree_bulk_extension_idx') /
             current_setting('block_size')::integer});
my $standby_pages = $standby->safe_psql(
	'postgres',
	q{SELECT pg_relation_size('btree_bulk_extension_idx') /
             current_setting('block_size')::integer});

cmp_ok($primary_pages, '>', $standby_pages,
	'reserved B-tree pages are not replayed on standby');

$standby->promote;
$standby->restart;

$standby->safe_psql(
	'postgres', q{
INSERT INTO btree_bulk_extension_test SELECT i FROM generate_series(10001, 20000) i;
});

is(
	$standby->safe_psql(
		'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM btree_bulk_extension_test WHERE i BETWEEN 1 AND 20000;
}),
	'20000',
	'promoted standby can extend and scan the B-tree');

done_testing();
