# Copyright (c) 2026, PostgreSQL Global Development Group

# A 32KB page can hold index tuples that exceed the prefix format's limits.
use strict;
use warnings FATAL => 'all';
use Digest::MD5 qw(md5_hex);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

plan skip_all => 'prefix representation limits require 32KB pages'
  unless $node->safe_psql('postgres', 'SHOW block_size') == 32768;

$node->safe_psql('postgres',
	'CREATE EXTENSION amcheck; CREATE EXTENSION pageinspect;');

my $small_hash = md5_hex(pack('H*', 'ab' x 1024));
my $large_hash = md5_hex(pack('H*', 'cd' x 8168));

# Check the structure and the exact indexed values, not just heap row counts.
# Each table has only the index under test when this is called.
sub check_index
{
	my ($table, $expected, $description) = @_;
	$node->safe_psql('postgres', "VACUUM ANALYZE $table");
	is( $node->safe_psql('postgres',
			"SELECT bt_index_parent_check('${table}_idx', true, true)"),
		'', "$description: amcheck");
	my $query = "SELECT a, count(*), octet_length(b), md5(b) FROM $table "
	  . 'GROUP BY a, b ORDER BY a';
	my $settings = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
	like(
		$node->safe_psql('postgres',
			$settings . 'EXPLAIN (COSTS OFF) ' . $query),
		qr/Index Only Scan using ${table}_idx/,
		"$description: index-only scan");
	is($node->safe_psql('postgres', $settings . $query),
		$expected, "$description: indexed values");
}

# Ten full anchors cannot fit in one directory IndexTuple, even though the
# page can hold them.  The 50-row case also exceeds a plain page's capacity.
$node->safe_psql(
	'postgres', q{
CREATE TABLE directory (a smallint, b bytea);
ALTER TABLE directory ALTER COLUMN b SET STORAGE PLAIN;
INSERT INTO directory
  SELECT a, decode(repeat('ab', 1024), 'hex')
  FROM generate_series(1, 10) a, generate_series(1, 3) b;
});
foreach my $copies (3, 5)
{
	$node->safe_psql(
		'postgres', q{
CREATE INDEX directory_idx ON directory (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
});
	check_index('directory',
		join("\n", map { "$_|$copies|1024|$small_hash" } 1 .. 10),
		'directory with ' . (10 * $copies) . ' rows');
	$node->safe_psql('postgres', 'DROP INDEX directory_idx');
	$node->safe_psql(
		'postgres', q{
INSERT INTO directory
  SELECT a, decode(repeat('ab', 1024), 'hex')
  FROM generate_series(1, 10) a, generate_series(1, 2) b;
}) if $copies == 3;
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE member (a smallint, b bytea);
ALTER TABLE member ALTER COLUMN b SET STORAGE PLAIN;
INSERT INTO member
  SELECT 1, decode(repeat('ab', 1024), 'hex') FROM generate_series(1, 3);
CREATE INDEX member_idx ON member (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
});
is( $node->safe_psql('postgres',
		"SELECT btpo_flags & 512 <> 0 FROM bt_page_stats('member_idx', 1)"),
	't', 'retail insertion starts with a coded leaf');

# Establish that the large key is legal in an ordinary index.
$node->safe_psql(
	'postgres', q{
CREATE TABLE plain (LIKE member);
ALTER TABLE plain ALTER COLUMN b SET STORAGE PLAIN;
INSERT INTO plain SELECT * FROM member;
CREATE INDEX plain_idx ON plain (a, b) WITH (deduplicate_items = off);
INSERT INTO plain VALUES (2, decode(repeat('cd', 8168), 'hex'));
});
is( $node->safe_psql('postgres',
		"SELECT max(itemlen) = 8184 FROM bt_page_items('plain_idx', 1)"),
	't', 'large plain tuple fits the index tuple size field');
check_index('plain', "1|3|1024|$small_hash\n2|1|8168|$large_hash",
	'plain control');

# With no shared bytes, the coded header pushes the size over INDEX_SIZE_MASK.
$node->safe_psql('postgres',
	"INSERT INTO member VALUES (2, decode(repeat('cd', 8168), 'hex'))");
check_index('member', "1|3|1024|$small_hash\n2|1|8168|$large_hash",
	'retail member limit');

# A dense coded leaf can fit a plain page without leaving BTMaxItemSize free.
# Force a representation-limit split before, between, and after its key groups.
foreach my $incoming (0, 2, 4)
{
	my $table = "dense_$incoming";
	$node->safe_psql('postgres', qq{
CREATE TABLE $table (a smallint, b bytea);
ALTER TABLE $table ALTER COLUMN b SET STORAGE PLAIN;
INSERT INTO $table
  SELECT CASE WHEN n <= 12 THEN 1 ELSE 3 END,
         decode(repeat('ab', 1024), 'hex') FROM generate_series(1, 23) n;
CREATE INDEX ${table}_idx ON $table (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
});
	is( $node->safe_psql('postgres', qq{
SELECT btpo_flags & 512 <> 0 AND btpo_level = 0 AND btpo_next = 0
  FROM bt_page_stats('${table}_idx', 1);
}), 't', "$table starts with a coded rightmost leaf");
	is( $node->safe_psql('postgres', qq{
SELECT decoded = 23 * (1040 + 4) AND decoded <= 32724
       AND 32724 - decoded < 10896 AND decoded + 8184 + 4 <= 32724
  FROM (SELECT sum(8 + octet_length(decode(replace(data, ' ', ''), 'hex')) + 4)
               AS decoded
          FROM bt_page_items('${table}_idx', 1) WHERE itemoffset > 1) s;
}), 't', "$table fits plain but lacks the maximum-item reserve");
	$node->safe_psql('postgres',
		"INSERT INTO $table VALUES ($incoming, decode(repeat('cd', 8168), 'hex'))");
	is( $node->safe_psql('postgres', qq{
SELECT btpo_next <> 0 FROM bt_page_stats('${table}_idx', 1);
}), 't', "$table splits for an unrepresentable coded member");
	my %expected = (1 => "1|12|1024|$small_hash",
		3 => "3|11|1024|$small_hash", $incoming => "$incoming|1|8168|$large_hash");
	check_index($table, join("\n", map { $expected{$_} } sort { $a <=> $b } keys %expected),
		"dense member insertion at $incoming");
}

# On little-endian hosts, 1 and 257 share one smallint byte.  The build
# estimator must reject the resulting oversized member too.
$node->safe_psql(
	'postgres', q{
DELETE FROM member WHERE a = 2;
DROP INDEX member_idx;
INSERT INTO member VALUES (257, decode(repeat('cd', 8168), 'hex'));
CREATE INDEX member_idx ON member (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
});
check_index('member', "1|3|1024|$small_hash\n257|1|8168|$large_hash",
	'build member limit');

# VACUUM removes the directory when the root becomes empty.
$node->safe_psql(
	'postgres', q{
TRUNCATE member;
DROP INDEX member_idx;
INSERT INTO member
  SELECT 1, decode(repeat('ab', 1024), 'hex') FROM generate_series(1, 3);
CREATE INDEX member_idx ON member (a, b)
  WITH (prefix_compression = on, deduplicate_items = off);
DELETE FROM member;
VACUUM member;
});
is( $node->safe_psql(
		'postgres', q{
SELECT btpo_flags & 512 = 0 AND btpo_flags & 2 <> 0
       AND live_items = 0 AND dead_items = 0
  FROM bt_page_stats('member_idx', 1);
}),
	't', 'vacuum removes the empty root directory');
$node->safe_psql('postgres',
	"INSERT INTO member VALUES (2, decode(repeat('cd', 8168), 'hex'))");
check_index('member', "2|1|8168|$large_hash", 'empty root insertion');

$node->stop;
done_testing();
