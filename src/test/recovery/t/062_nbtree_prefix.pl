# Copyright (c) 2026, PostgreSQL Global Development Group

# Replay of prefix-compressed btree leaves on a streaming standby.
#
# Redo rebuilds coded leaves from the WAL record, not from a full-page image,
# for inserts, splits, deduplication, posting list splits, VACUUM, simple and
# bottom-up deletion, and page deletion.  wal_consistency_checking makes any
# divergence from the primary's page fail replay.
#
# Queries test btpo_flags bits from nbtree.h: BTP_ROOT is 2, BTP_PREFIX is 512.
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
CREATE EXTENSION pageinspect;

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

-- Two variable-length key columns: rows share the whole first column and a
-- long, length-varying prefix of the second, so coded leaves carry a nonzero
-- fully-shared leading span (lead_shared > 0).  Replaying their inserts and
-- splits under wal_consistency_checking exercises the multi-column decode.
CREATE TABLE pfx_mc (a text COLLATE "C", b text COLLATE "C");
CREATE INDEX pfx_mc_ab ON pfx_mc (a, b) WITH (prefix_compression = on);
INSERT INTO pfx_mc
  SELECT 'https://example.com/tenant/' || lpad((g % 50)::text, 4, '0'),
         '/path/segment/' || repeat('y', 64) || '/' || g
  FROM generate_series(1, 20000) g;
INSERT INTO pfx_mc
  SELECT 'https://example.com/tenant/' || lpad((g % 50)::text, 4, '0'),
         '/path/segment/' || repeat('y', 64) || '/' || g
  FROM generate_series(20001, 40000) g ORDER BY random();

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

-- Non-key updates: bottom-up deletion on coded pages.
CREATE TABLE pfx_upd (id int PRIMARY KEY, u text COLLATE "C", f int);
CREATE INDEX pfx_upd_u ON pfx_upd (u) WITH (prefix_compression = on);
INSERT INTO pfx_upd
  SELECT g, 'https://example.com/x/' || lpad((g % 500)::text, 8, '0'), 0
  FROM generate_series(1, 20000) g;
UPDATE pfx_upd SET f = f + 1;
UPDATE pfx_upd SET f = f + 1;

-- Unique checking with a nullable second key and an INCLUDE column.  Equal
-- numeric values with different scales must retain their representations.
CREATE TABLE pfx_unique (u text COLLATE "C", n numeric, payload text,
                         f int DEFAULT 0);
INSERT INTO pfx_unique (u, n, payload)
  SELECT 'https://example.com/unique/' || lpad(g::text, 8, '0'),
         CASE WHEN g % 10 = 0 THEN NULL
              WHEN g % 2 = 0 THEN 1.0 ELSE 1.00 END,
         CASE WHEN g % 7 = 0 THEN NULL ELSE 'payload-' || g END
  FROM generate_series(1, 2000) g;
CREATE UNIQUE INDEX pfx_unique_u ON pfx_unique (u, n) INCLUDE (payload)
  NULLS NOT DISTINCT WITH (prefix_compression = on);
INSERT INTO pfx_unique (u, n, payload)
  SELECT 'https://example.com/unique/' || lpad(g::text, 8, '0'),
         CASE WHEN g % 10 = 0 THEN NULL
              WHEN g % 2 = 0 THEN 1.0 ELSE 1.00 END,
         CASE WHEN g % 7 = 0 THEN NULL ELSE 'payload-' || g END
  FROM generate_series(2001, 4000) g;
VACUUM ANALYZE pfx_unique;
});

# Check the leaves containing both duplicate probe keys, not unrelated leaves
# that may be plain.  Do this before updates can create HOT root redirects.
is( $primary->safe_psql(
		'postgres', q{
SELECT count(DISTINCT u) = 2
  FROM generate_series(1,
       (pg_relation_size('pfx_unique_u') /
        current_setting('block_size')::int)::int - 1) b,
       LATERAL bt_page_stats('pfx_unique_u', b) s,
       LATERAL bt_page_items('pfx_unique_u', b) i,
       pfx_unique t
  WHERE s.type = 'l' AND (s.btpo_flags & 512) <> 0
    AND i.itemoffset > CASE WHEN s.btpo_next = 0 THEN 1 ELSE 2 END
    AND i.htid = t.ctid
    AND t.u IN ('https://example.com/unique/00000002',
                'https://example.com/unique/00000010');
}),
	't',
	'both unique duplicate probes target prefix-compressed leaves');

foreach my $duplicate (
	q{('https://example.com/unique/00000002', 1.000, 'duplicate')},
	q{('https://example.com/unique/00000010', NULL, 'duplicate')})
{
	my ($ret, $stdout, $stderr) = $primary->psql('postgres',
		"INSERT INTO pfx_unique (u, n, payload) VALUES $duplicate");
	is($ret, 3, 'compressed unique index rejects duplicate');
	like(
		$stderr,
		qr/duplicate key value violates unique constraint "pfx_unique_u"/,
		'duplicate rejected by compressed index');
}

$primary->safe_psql(
	'postgres', q{
UPDATE pfx_unique SET f = f + 1;
UPDATE pfx_unique SET f = f + 1;
VACUUM ANALYZE pfx_unique;
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
SELECT count(*), md5(string_agg(a || E'\x1f' || b, '|' ORDER BY a, b))
  FROM pfx_mc;
};
is( $standby->safe_psql('postgres', $query),
	$primary->safe_psql('postgres', $query),
	'standby index scans match primary');

# Compare the decoded keys and INCLUDE values with an independent heap scan,
# including numeric scale (numeric equality alone would not detect its loss).
my $unique_query = q{
SELECT count(*), md5(string_agg(
  u || ':' || coalesce(n::text, '<null>') || ':' ||
  coalesce(payload, '<null>'), '|' ORDER BY u)) FROM pfx_unique;
};
my $unique_expected = $primary->safe_psql(
	'postgres',
	'SET enable_indexscan = off; SET enable_indexonlyscan = off; '
	  . 'SET enable_bitmapscan = off; SET enable_seqscan = on; '
	  . $unique_query);
foreach my $node ($primary, $standby)
{
	my $ios = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
	  . 'SET enable_indexonlyscan = on; ';
	like(
		$node->safe_psql('postgres',
			$ios . 'EXPLAIN (COSTS OFF) ' . $unique_query),
		qr/Index Only Scan using pfx_unique_u/,
		'unique index digest uses index-only scan');
	is($node->safe_psql('postgres', $ios . $unique_query),
		$unique_expected, 'unique index preserves keys and INCLUDE values');
	is( $node->safe_psql(
			'postgres', $ios . q{
SELECT n::text, payload FROM pfx_unique
  WHERE u = ANY (ARRAY['https://example.com/unique/00000002',
                       'https://example.com/unique/00000003',
                       'https://example.com/unique/00002002'])
  ORDER BY u;
SELECT payload FROM pfx_unique
  WHERE u = ANY (ARRAY['https://example.com/unique/00000010',
                       'https://example.com/unique/00002010'])
    AND n IS NULL ORDER BY u;
}),
		"1.0|payload-2\n1.00|payload-3\n1.0|\npayload-10\npayload-2010",
		'array probes and NULL keys on multicolumn unique index');
}

foreach my $idx (qw(pfx_url_u pfx_url_u_id pfx_dup_u pfx_upd_u pfx_unique_u
	pfx_mc_ab))
{
	is( $standby->safe_psql(
			'postgres', "SELECT bt_index_check('$idx', true)"),
		'',
		"amcheck on standby: $idx");
}

# The two-varlena-column index must have coded leaves that carry a nonzero
# fully-shared leading span; otherwise the replay above never exercised the
# multi-column decode path.
ok( $primary->safe_psql(
		'postgres', q{
SELECT count(*) > 0
  FROM generate_series(1,
       (pg_relation_size('pfx_mc_ab') /
        current_setting('block_size')::int)::int - 1) b,
       LATERAL bt_page_stats('pfx_mc_ab', b) s
  WHERE s.type = 'l' AND (s.btpo_flags & 512) <> 0;
}) eq 't',
	'multicolumn index has prefix-compressed leaves');

# Guard against a workload that never produced a coded leaf.
ok( $primary->safe_psql(
		'postgres', q{
SELECT count(*) > 0
  FROM generate_series(1,
       (pg_relation_size('pfx_url_u') /
        current_setting('block_size')::int)::int - 1) b,
       LATERAL bt_page_stats('pfx_url_u', b) s
  WHERE s.type = 'l' AND (s.btpo_flags & 512) <> 0;
}) eq 't',
	'index has prefix-compressed leaves');

# VACUUM removes the empty root directory on both primary and standby.
SKIP:
{
	skip 'large tuple insertion into empty root requires 32KB pages', 7
	  unless $primary->safe_psql('postgres', 'SHOW block_size') == 32768;

	$primary->safe_psql(
		'postgres', q{
CREATE TABLE pfx_empty (a smallint, b bytea);
ALTER TABLE pfx_empty ALTER COLUMN b SET STORAGE PLAIN;
INSERT INTO pfx_empty
  SELECT 1, decode(repeat('ab', 1024), 'hex') FROM generate_series(1, 3);
CREATE INDEX pfx_empty_idx ON pfx_empty (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
CHECKPOINT;
DELETE FROM pfx_empty;
VACUUM pfx_empty;
});
	is( $primary->safe_psql(
			'postgres', q{
SELECT btpo_flags & 512 = 0 AND btpo_flags & 2 <> 0
       AND live_items = 0 AND dead_items = 0
  FROM bt_page_stats('pfx_empty_idx', 1);
}),
		't', 'vacuum removes the empty root directory before insertion');

	$primary->safe_psql(
		'postgres', q{
INSERT INTO pfx_empty VALUES (2, decode(repeat('cd', 8168), 'hex'));
VACUUM pfx_empty;
});
	$primary->wait_for_replay_catchup($standby);
	foreach my $node ($primary, $standby)
	{
		is( $node->safe_psql('postgres',
				"SELECT bt_index_check('pfx_empty_idx', true)"),
			'', 'empty root insertion passes amcheck');
		my $settings = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
		my $query = q{
SELECT a, octet_length(b), b = decode(repeat('cd', 8168), 'hex')
  FROM pfx_empty;
};
		like(
			$node->safe_psql('postgres',
				$settings . 'EXPLAIN (COSTS OFF) ' . $query),
			qr/Index Only Scan using pfx_empty_idx/,
			'empty root insertion uses index-only scan');
		is($node->safe_psql('postgres', $settings . $query),
			'2|8168|t', 'empty root insertion preserves indexed value');
	}
}

# Deduplication merges keys that are equal under equalimage even when their
# stored bytes differ.  Here the heap compresses equal keys with pglz first
# and lz4 later, and index tuples keep the inline-compressed datums, so
# posting lists built and split on coded leaves mix both representations.
# The index must still return the stored values, on the primary and after
# byte-checked replay on the standby.
SKIP:
{
	skip 'mixed compression methods require lz4', 7
	  unless check_pg_config('^#define USE_LZ4 1$');

	$primary->safe_psql(
		'postgres', q{
CREATE TABLE pfx_img (id int, k text COLLATE "C")
  WITH (autovacuum_enabled = off, fillfactor = 100);
ALTER TABLE pfx_img ALTER COLUMN k SET COMPRESSION pglz;
INSERT INTO pfx_img
  SELECT g, 'https://example.com/path/to/resource/' ||
            lpad((g % 20)::text, 4, '0') || repeat('x', 3000)
  FROM generate_series(1, 400) g;
CREATE INDEX pfx_img_k ON pfx_img (k) WITH (prefix_compression = on);
-- Free heap slots inside every posting list's TID range, so that the lz4
-- reinserts split existing posting lists instead of only appending.
DELETE FROM pfx_img WHERE id BETWEEN 150 AND 250;
VACUUM pfx_img;
ALTER TABLE pfx_img ALTER COLUMN k SET COMPRESSION lz4;
INSERT INTO pfx_img
  SELECT g, 'https://example.com/path/to/resource/' ||
            lpad((g % 20)::text, 4, '0') || repeat('x', 3000)
  FROM generate_series(150, 800) g WHERE g NOT BETWEEN 251 AND 400;
VACUUM ANALYZE pfx_img;
});
	is( $primary->safe_psql(
			'postgres', q{
SELECT count(*) > 0
  FROM (SELECT b, i.itemoffset
          FROM generate_series(1,
               (pg_relation_size('pfx_img_k') /
                current_setting('block_size')::int)::int - 1) b,
               LATERAL bt_page_stats('pfx_img_k', b) s,
               LATERAL bt_page_items('pfx_img_k', b) i,
               LATERAL unnest(i.tids) t(tid)
          JOIN pfx_img h ON h.ctid = t.tid
         WHERE s.type = 'l' AND (s.btpo_flags & 512) <> 0
         GROUP BY 1, 2
        HAVING count(DISTINCT pg_column_compression(h.k)) = 2) m;
}),
		't',
		'coded posting lists merge pglz and lz4 representations');

	$primary->wait_for_replay_catchup($standby);
	my $query = q{SELECT count(*), md5(string_agg(k, '|' ORDER BY k))
  FROM pfx_img};
	my $expected = $primary->safe_psql('postgres',
		'SET enable_indexscan = off; SET enable_indexonlyscan = off; '
		  . 'SET enable_bitmapscan = off; ' . $query);
	foreach my $node ($primary, $standby)
	{
		is( $node->safe_psql('postgres',
				"SELECT bt_index_check('pfx_img_k', true)"),
			'', 'mixed-representation index passes amcheck');
		my $ios = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
		like(
			$node->safe_psql('postgres', $ios . 'EXPLAIN (COSTS OFF) ' . $query),
			qr/Index Only Scan using pfx_img_k/,
			'mixed-representation digest uses index-only scan');
		is($node->safe_psql('postgres', $ios . $query),
			$expected, 'mixed-representation index returns the heap values');
	}
}

# A nondeterministic collation has no equalimage support, so deduplication
# stays off while prefix compression shares the identical bytes of keys that
# compare equal but are stored differently.  Each case variant must come back
# unchanged, and uniqueness must still use the collation's equality.
SKIP:
{
	skip 'nondeterministic collations require ICU', 9
	  unless check_pg_config('^#define USE_ICU 1$');

	# ICU collations need a UTF8 database, whatever the cluster's default.
	$primary->safe_psql('postgres',
		"CREATE DATABASE pfx_icu ENCODING 'UTF8' LOCALE 'C' TEMPLATE template0"
	);
	$primary->safe_psql(
		'pfx_icu', q{
CREATE EXTENSION amcheck;
CREATE EXTENSION pageinspect;
CREATE COLLATION pfx_ci (provider = icu, locale = 'und-u-ks-level2',
                         deterministic = false);
CREATE TABLE pfx_nd (k text COLLATE pfx_ci) WITH (autovacuum_enabled = off);
INSERT INTO pfx_nd
  SELECT 'https://example.com/path/to/resource/' || lpad(g::text, 5, '0')
         || v
  FROM generate_series(1, 1500) g,
       unnest(ARRAY['/view', '/VIEW', '/View']) v;
CREATE INDEX pfx_nd_k ON pfx_nd (k) WITH (prefix_compression = on);
INSERT INTO pfx_nd
  SELECT 'https://example.com/path/to/resource/' || lpad(g::text, 5, '0')
         || '/vIeW'
  FROM generate_series(1, 1500) g;
CREATE TABLE pfx_nd_u (k text COLLATE pfx_ci) WITH (autovacuum_enabled = off);
INSERT INTO pfx_nd_u
  SELECT 'https://example.com/path/to/resource/' || lpad(g::text, 5, '0')
  FROM generate_series(1, 3000) g;
CREATE UNIQUE INDEX pfx_nd_u_k ON pfx_nd_u (k)
  WITH (prefix_compression = on);
VACUUM ANALYZE pfx_nd;
VACUUM ANALYZE pfx_nd_u;
});
	is( $primary->safe_psql(
			'pfx_icu', q{
SELECT NOT m.allequalimage AND
       bool_or(s.type = 'l' AND (s.btpo_flags & 512) <> 0) AND
       NOT bool_or(i.tids IS NOT NULL)
  FROM bt_metap('pfx_nd_k') m,
       generate_series(1,
       (pg_relation_size('pfx_nd_k') /
        current_setting('block_size')::int)::int - 1) b,
       LATERAL bt_page_stats('pfx_nd_k', b) s,
       LATERAL bt_page_items('pfx_nd_k', b) i
 GROUP BY m.allequalimage;
}),
		't',
		'nondeterministic index is coded without posting lists');

	my ($ret, $stdout, $stderr) = $primary->psql('pfx_icu',
		"INSERT INTO pfx_nd_u VALUES ('HTTPS://EXAMPLE.COM/PATH/TO/RESOURCE/01234')");
	is($ret, 3, 'case variant of an existing key is rejected');
	like(
		$stderr,
		qr/duplicate key value violates unique constraint "pfx_nd_u_k"/,
		'nondeterministic uniqueness is checked on coded leaves');

	$primary->wait_for_replay_catchup($standby);
	my $query = q{SELECT count(*), md5(string_agg(k, '|' ORDER BY k COLLATE "C"))
  FROM pfx_nd};
	my $expected = $primary->safe_psql('pfx_icu',
		'SET enable_indexscan = off; SET enable_indexonlyscan = off; '
		  . 'SET enable_bitmapscan = off; ' . $query);
	my $ios = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
	foreach my $node ($primary, $standby)
	{
		is( $node->safe_psql('pfx_icu',
				"SELECT bt_index_check('pfx_nd_k', true)"),
			'', 'nondeterministic index passes amcheck');
		is($node->safe_psql('pfx_icu', $ios . $query),
			$expected, 'nondeterministic index returns every variant unchanged');
	}
	like(
		$primary->safe_psql('pfx_icu', $ios . 'EXPLAIN (COSTS OFF) ' . $query),
		qr/Index Only Scan using pfx_nd_k/,
		'nondeterministic digest uses index-only scan');
	is( $standby->safe_psql(
			'pfx_icu', $ios . q{
SELECT string_agg(k, ',' ORDER BY k COLLATE "C") FROM pfx_nd
 WHERE k = 'https://example.com/path/to/resource/00042/view';
}),
		'https://example.com/path/to/resource/00042/VIEW,'
		  . 'https://example.com/path/to/resource/00042/View,'
		  . 'https://example.com/path/to/resource/00042/vIeW,'
		  . 'https://example.com/path/to/resource/00042/view',
		'equality probe finds all case variants');
}

done_testing();
