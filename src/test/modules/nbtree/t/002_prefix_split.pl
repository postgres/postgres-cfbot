# Copyright (c) 2026, PostgreSQL Global Development Group

# Compare rightmost splits from identical coded pages, not differently built trees.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

plan skip_all => 'prefix split fixture requires 8KB pages and 8-byte alignment'
  unless $node->safe_psql('postgres',
	'SELECT database_block_size = 8192 AND max_data_alignment = 8 FROM pg_control_init()')
  eq 't';

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
CREATE EXTENSION pageinspect;
CREATE FUNCTION make_key(n integer) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT '/srv/objects/customer-0042/archive/' || lpad(n::text, 9, '0') ||
         '/' || repeat('a', 96) || '.json'
$$;
CREATE TABLE low (k text COLLATE "C");
CREATE INDEX low_idx ON low (k) WITH (prefix_compression = on);
CREATE TABLE high (LIKE low);
CREATE INDEX high_idx ON high (k) WITH (prefix_compression = on);
DO $$
BEGIN
  FOR n IN 1..361 LOOP
    INSERT INTO low VALUES (make_key((n - 1) / 4));
    INSERT INTO high VALUES (make_key((n - 1) / 4));
  END LOOP;
END
$$;
});

foreach my $table ('low', 'high')
{
	is( $node->safe_psql('postgres', qq{
SELECT btpo_flags & 512 <> 0 AND btpo_level = 0 AND btpo_next = 0
  FROM bt_page_stats('${table}_idx', 2);
}), 't', "$table starts with a coded rightmost leaf");
}

# Ignore only the WAL/checksum portion of the page header.
is( $node->safe_psql('postgres', q{
SELECT substring(get_raw_page('low_idx', 2) FROM 13) =
       substring(get_raw_page('high_idx', 2) FROM 13);
}), 't', 'fillfactor comparison starts with identical page contents');

# Even merging every equal key into a posting list cannot produce a plain
# page.  The split must use the decoded-item array, not a temporary plain page.
is( $node->safe_psql('postgres', q{
SELECT sum(((8 + octet_length(decode(replace(data, ' ', ''), 'hex')) +
             CASE WHEN n > 1 THEN n * 6 ELSE 0 END + 7) / 8) * 8 + 4) >
       current_setting('block_size')::integer
  FROM (SELECT data, sum(coalesce(cardinality(tids), 1)) n
          FROM bt_page_items('low_idx', 2)
         WHERE itemoffset > 1 GROUP BY data) s;
}), 't', 'decoded leaf exceeds a page even after deduplication');

$node->safe_psql('postgres', q{
ALTER INDEX low_idx SET (fillfactor = 10);
ALTER INDEX high_idx SET (fillfactor = 90);
INSERT INTO low VALUES (make_key(361 / 4));
INSERT INTO high VALUES (make_key(361 / 4));
});

my %left_tids;
foreach my $table ('low', 'high')
{
	is( $node->safe_psql('postgres', qq{
SELECT btpo_next <> 0 FROM bt_page_stats('${table}_idx', 2);
}), 't', "$table splits on the same insertion");
	$left_tids{$table} = $node->safe_psql('postgres', qq{
SELECT sum(coalesce(cardinality(tids), 1))
  FROM bt_page_items('${table}_idx', 2), bt_page_stats('${table}_idx', 2)
 WHERE itemoffset > 1 + CASE WHEN btpo_flags & 512 <> 0 THEN 1 ELSE 0 END;
});
	$node->safe_psql('postgres', "VACUUM ANALYZE $table");
	is( $node->safe_psql('postgres',
			"SELECT bt_index_parent_check('${table}_idx', true, true)"),
		'', "$table: amcheck");
	my $query = "SELECT md5(string_agg(k, ',' ORDER BY k)) FROM $table";
	my $expected = $node->safe_psql('postgres',
		"SELECT md5(string_agg(make_key((n - 1) / 4), ',' ORDER BY n)) "
		  . 'FROM generate_series(1, 362) n');
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
