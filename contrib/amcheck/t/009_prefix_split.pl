# Copyright (c) 2026, PostgreSQL Global Development Group

# Fillfactor must steer a rightmost split of a coded leaf.  Both indexes split
# byte-identical pages, so only fillfactor differs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'prefix split fixture requires 8KB pages and 8-byte alignment'
  unless check_pg_config('^#define BLCKSZ 8192$')
  && check_pg_config('^#define MAXIMUM_ALIGNOF 8$');

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Each key shares the long leading value, so a leaf codes tightly.  The build
# loads 507 distinct keys with four duplicates each; deduplication merges each
# key's duplicates into one posting list, and the rightmost leaf ends coded,
# one directory short of a split.  make_key(507..511) fill it; make_key(512)
# tips it.  bt_page_stats block numbers are stable for this fixed build.
my $distinct = 507;

$node->safe_psql(
	'postgres', qq{
CREATE EXTENSION amcheck;
CREATE EXTENSION pageinspect;
CREATE FUNCTION make_key(n integer) RETURNS text LANGUAGE sql IMMUTABLE AS \$\$
  SELECT '/srv/objects/customer-0042/archive/' || lpad(n::text, 9, '0') ||
         '/' || repeat('a', 96) || '.json'
\$\$;
CREATE TABLE low (k text COLLATE "C");
INSERT INTO low SELECT make_key(g / 4)
  FROM generate_series(0, $distinct * 4 - 1) g;
CREATE INDEX low_idx ON low (k) WITH (prefix_compression = on);
CREATE TABLE high (LIKE low);
INSERT INTO high SELECT make_key(g / 4)
  FROM generate_series(0, $distinct * 4 - 1) g;
CREATE INDEX high_idx ON high (k) WITH (prefix_compression = on);
});

# Find the rightmost leaf (btpo_next = 0) by block number rather than assuming
# an offset, so the test survives directory-layout changes.
my $rightblk = sub {
	my ($table) = @_;
	return $node->safe_psql('postgres', qq{
SELECT b FROM generate_series(1, pg_relation_size('${table}_idx')/8192 - 1) b,
       LATERAL bt_page_stats('${table}_idx', b) s
 WHERE s.btpo_level = 0 AND s.btpo_next = 0;
});
};

foreach my $table ('low', 'high')
{
	my $blk = $rightblk->($table);
	is( $node->safe_psql('postgres', qq{
SELECT btpo_flags & 512 <> 0 AND btpo_level = 0
  FROM bt_page_stats('${table}_idx', $blk);
}), 't', "$table rightmost leaf is coded");
}

# Skip pd_lsn, pd_checksum and pd_flags, which differ between the two pages.
my $lowblk = $rightblk->('low');
my $highblk = $rightblk->('high');
is( $node->safe_psql('postgres', qq{
SELECT substring(get_raw_page('low_idx', $lowblk) FROM 13) =
       substring(get_raw_page('high_idx', $highblk) FROM 13);
}), 't', 'fillfactor comparison starts with identical page contents');

# Even merging every equal key into a posting list cannot produce a plain
# page.  The split must use the decoded-item array, not a temporary plain page.
is( $node->safe_psql('postgres', qq{
SELECT sum(((8 + octet_length(decode(replace(data, ' ', ''), 'hex')) +
             CASE WHEN n > 1 THEN n * 6 ELSE 0 END + 7) / 8) * 8 + 4) >
       current_setting('block_size')::integer
  FROM (SELECT data, sum(coalesce(cardinality(tids), 1)) n
          FROM bt_page_items('low_idx', $lowblk)
         WHERE itemoffset > 1 GROUP BY data) s;
}), 't', 'decoded leaf exceeds a page even after deduplication');

# Fill the rightmost leaf one directory short of a split, then tip both at the
# same insertion.  Only fillfactor differs across the two indexes.
$node->safe_psql('postgres', qq{
ALTER INDEX low_idx SET (fillfactor = 10);
ALTER INDEX high_idx SET (fillfactor = 90);
INSERT INTO low  SELECT make_key(g) FROM generate_series($distinct, 511) g;
INSERT INTO high SELECT make_key(g) FROM generate_series($distinct, 511) g;
});

my %left_tids;
foreach my $table ('low', 'high')
{
	my $blk = $table eq 'low' ? $lowblk : $highblk;
	is( $node->safe_psql('postgres', qq{
SELECT btpo_next = 0 FROM bt_page_stats('${table}_idx', $blk);
}), 't', "$table rightmost leaf is one insertion short of a split");
}

$node->safe_psql('postgres', q{
INSERT INTO low  VALUES (make_key(512));
INSERT INTO high VALUES (make_key(512));
});

foreach my $table ('low', 'high')
{
	my $blk = $table eq 'low' ? $lowblk : $highblk;
	is( $node->safe_psql('postgres', qq{
SELECT btpo_next <> 0 FROM bt_page_stats('${table}_idx', $blk);
}), 't', "$table splits on the same insertion");
	$left_tids{$table} = $node->safe_psql('postgres', qq{
SELECT sum(coalesce(cardinality(tids), 1))
  FROM bt_page_items('${table}_idx', $blk), bt_page_stats('${table}_idx', $blk)
 WHERE itemoffset > 1 + CASE WHEN btpo_flags & 512 <> 0 THEN 1 ELSE 0 END;
});
	$node->safe_psql('postgres', "VACUUM ANALYZE $table");
	is( $node->safe_psql('postgres',
			"SELECT bt_index_parent_check('${table}_idx', true, true)"),
		'', "$table: amcheck");
	my $query = "SELECT md5(string_agg(k, ',' ORDER BY k)) FROM $table";
	my $expected = $node->safe_psql('postgres',
		"SELECT md5(string_agg(k, ',' ORDER BY k)) FROM ("
		  . "SELECT make_key(g / 4) AS k"
		  . " FROM generate_series(0, $distinct * 4 - 1) g"
		  . " UNION ALL SELECT make_key(g)"
		  . " FROM generate_series($distinct, 512) g) s");
	my $settings = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
	like(
		$node->safe_psql('postgres',
			$settings . 'EXPLAIN (COSTS OFF) ' . $query),
		qr/Index Only Scan using ${table}_idx/,
		"$table: index-only scan");
	is($node->safe_psql('postgres', $settings . $query),
		$expected, "$table: indexed values");
}

cmp_ok($left_tids{high}, '>', $left_tids{low},
	'higher fillfactor leaves more TIDs on the finalized left page');

$node->stop;
done_testing();
