# Copyright (c) 2026, PostgreSQL Global Development Group

# Concurrent hash index inserts defer their tuple count when they find the
# metapage locked.  Check that such inserts replay correctly on a standby,
# that the index still splits as it grows, and that the deferred count stays
# close to the real one even when sessions are short.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

$primary->safe_psql(
	'postgres', q{
CREATE TABLE hash_conc (v int4);
CREATE INDEX hash_conc_idx ON hash_conc USING hash (v);
});

my $meta = q{SELECT ntuples, maxbucket
	FROM hash_metapage_info(get_raw_page('hash_conc_idx', 0))};
my (undef, $maxbucket0) = split /\|/, $primary->safe_psql('postgres', $meta);

# Long sessions, and short ones that exit with tuples pending
$primary->pgbench(
	'--no-vacuum --client=8 --jobs=8 --transactions=2000',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent inserts',
	{
		'hash_conc_long' => q{
\set v random(1, 1000000000)
INSERT INTO hash_conc VALUES (:v);
INSERT INTO hash_conc VALUES (:v + 1);
}
	});
$primary->pgbench(
	'--no-vacuum --client=8 --jobs=8 --transactions=200 --connect',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent short sessions',
	{
		'hash_conc_short' => q{
\set v random(1, 1000000000)
INSERT INTO hash_conc VALUES (:v);
}
	});

my $nrows = $primary->safe_psql('postgres', 'SELECT count(*) FROM hash_conc');
is($nrows, 8 * 2000 * 2 + 8 * 200, 'all rows inserted');

my ($ntuples, $maxbucket) =
  split /\|/, $primary->safe_psql('postgres', $meta);
cmp_ok($maxbucket, '>', $maxbucket0, 'index split');
cmp_ok($ntuples, '<=', $nrows, 'tuple count not too high');
# A session leaves fewer than 256 tuples uncounted, and a single-insert
# session at most one
cmp_ok(
	$ntuples, '>=',
	$nrows - (8 * 256 + 8 * 200),
	'tuple count not too low');

$primary->wait_for_replay_catchup($standby);

is($standby->safe_psql('postgres', $meta),
	"$ntuples|$maxbucket", 'standby metapage matches primary');

my $probe = q{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM hash_conc h
  WHERE (SELECT count(*) FROM hash_conc i WHERE i.v = h.v) =
        (SELECT count(*) FROM hash_conc s WHERE s.v + 0 = h.v)
  AND h.v % 97 = 0};
my $expected = $primary->safe_psql('postgres',
	'SELECT count(*) FROM hash_conc WHERE v % 97 = 0');
is($primary->safe_psql('postgres', $probe),
	$expected, 'index probes match heap on primary');
is($standby->safe_psql('postgres', $probe),
	$expected, 'index probes match heap on standby');

done_testing();
