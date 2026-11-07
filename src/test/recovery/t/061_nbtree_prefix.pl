
# Copyright (c) 2026, PostgreSQL Global Development Group

# Replay of prefix-compressed btree leaves (the prefix_compression index
# storage parameter) on a streaming standby.
#
# Every page change on a BTP_PREFIX leaf -- in-place inserts of coded tuples,
# leaf splits whose halves redo lays out in coded form, deduplication,
# posting list splits, VACUUM, simple and bottom-up deletion, page deletion --
# is replayed by redo routines that rebuild the coded page from the WAL record
# without a full-page image.  wal_consistency_checking compares each replayed
# page with the primary's, so any divergence fails replay on the standby.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
wal_consistency_checking = 'btree'
autovacuum = off
});
$primary->start;

# wal_consistency_checking logs a full-page image with every btree record, so
# the workload writes far more WAL than max_wal_size; a slot keeps the primary
# from recycling segments the standby has not received yet.
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('standby')");
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'standby'");
$standby->start;

$primary->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;

-- Distinct keys sharing a long prefix: coded inserts and splits, including
-- splits of pages whose decoded content exceeds one page.
CREATE TABLE pfx_url (u text COLLATE "C", id int);
CREATE INDEX pfx_url_u ON pfx_url (u) WITH (prefix_compression = on);
INSERT INTO pfx_url
  SELECT 'https://example.com/path/to/resource/' || (hashint4(g) & x'7fffffff'::int), g
  FROM generate_series(1, 30000) g;

-- Bulk-built coded leaves, then inserts into them.
CREATE INDEX pfx_url_u_id ON pfx_url (u, id) WITH (prefix_compression = on);
INSERT INTO pfx_url
  SELECT 'https://example.com/path/to/resource/' || g, -g
  FROM generate_series(1, 10000) g;

-- Duplicates in random order: deduplication of coded pages, then posting
-- list splits on them.
CREATE TABLE pfx_dup (u text COLLATE "C", n int);
CREATE INDEX pfx_dup_u ON pfx_dup (u) WITH (prefix_compression = on);
INSERT INTO pfx_dup
  SELECT 'https://example.com/path/to/resource/' || lpad((g % 300)::text, 8, '0'), g
  FROM generate_series(1, 30000) g ORDER BY random();
INSERT INTO pfx_dup
  SELECT 'https://example.com/path/to/resource/' || lpad((g % 300)::text, 8, '0'), g
  FROM generate_series(30001, 60000) g ORDER BY random();

-- VACUUM of coded pages (posting list updates and deletions), then page
-- deletion of emptied coded leaves.
DELETE FROM pfx_dup WHERE n % 3 = 0;
VACUUM pfx_dup;
DELETE FROM pfx_dup WHERE u < 'https://example.com/path/to/resource/00000150';
VACUUM pfx_dup;

-- Non-key updates: bottom-up deletion on coded pages of a unique index.
CREATE TABLE pfx_upd (id int PRIMARY KEY, u text COLLATE "C", f int);
CREATE INDEX pfx_upd_u ON pfx_upd (u) WITH (prefix_compression = on);
INSERT INTO pfx_upd
  SELECT g, 'https://example.com/x/' || lpad((g % 500)::text, 8, '0'), 0
  FROM generate_series(1, 20000) g;
UPDATE pfx_upd SET f = f + 1;
UPDATE pfx_upd SET f = f + 1;
});

$primary->wait_for_replay_catchup($standby);

# The standby replayed everything (a consistency failure would have stopped
# it) and is still serving queries.
is($standby->safe_psql('postgres', 'SELECT 1'), '1', 'standby is alive');

my $query = q{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*), md5(string_agg(u, '|' ORDER BY u)) FROM pfx_url;
SELECT count(*), md5(string_agg(u || n, '|' ORDER BY u, n)) FROM pfx_dup;
SELECT count(*) FROM pfx_upd WHERE u = 'https://example.com/x/00000007';
};
is( $standby->safe_psql('postgres', $query),
	$primary->safe_psql('postgres', $query),
	'standby index scans match primary');

# The replayed indexes are structurally valid.
foreach my $idx (qw(pfx_url_u pfx_url_u_id pfx_dup_u pfx_upd_u))
{
	is( $standby->safe_psql(
			'postgres', "SELECT bt_index_check('$idx', true)"),
		'',
		"amcheck on standby: $idx");
}

# The workload did use prefix-compressed leaves.
ok( $primary->safe_psql(
		'postgres', q{
CREATE EXTENSION pageinspect;
SELECT count(*) > 0
  FROM generate_series(1, (pg_relation_size('pfx_url_u') / 8192)::int - 1) b,
       LATERAL bt_page_stats('pfx_url_u', b) s
  WHERE s.type = 'l' AND (s.btpo_flags & 512) <> 0;
}) eq 't',
	'index has prefix-compressed leaves');

done_testing();
