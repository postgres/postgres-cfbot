# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub marker_count
{
	my ($node, $index) = @_;

	return $node->safe_psql(
		'postgres',
		qq[
SELECT count(*)
FROM generate_series(0,
       (SELECT relpages - 1 FROM pg_class WHERE oid = '$index'::regclass)) AS b
CROSS JOIN LATERAL
     gist_page_items_bytea(get_raw_page('$index', b)) AS i
WHERE i.ctid::text LIKE '(4294967295,%'
]);
}

sub exact_result
{
	my ($node, $enable_seqscan) = @_;

	return $node->safe_psql(
		'postgres',
		qq[
SET enable_seqscan = $enable_seqscan;
SET enable_bitmapscan = off;
SELECT string_agg(id::text, ',' ORDER BY id)
FROM gist_intrapage
WHERE p <@ box(point(80, 80), point(20, 20));
]);
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'wal_consistency_checking = gist');
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

my $backup_name = 'gist_intrapage_backup';
$primary->backup($backup_name);
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->safe_psql(
	'postgres', q[
CREATE TABLE gist_intrapage AS
SELECT i AS id, point(i % 200, i / 200) AS p
FROM generate_series(1, 40000) AS i;

CREATE INDEX gist_intrapage_sorted ON gist_intrapage USING gist (p);
]);

cmp_ok(marker_count($primary, 'gist_intrapage_sorted'), '>', 0,
	'sorted build creates skip tuples');
is(exact_result($primary, 'off'), exact_result($primary, 'on'),
	'skip tuples preserve the exact scan result');

$primary->safe_psql(
	'postgres', q[
CREATE INDEX gist_intrapage_buffered ON gist_intrapage USING gist (p)
  WITH (buffering = on, fillfactor = 50);
]);
cmp_ok(marker_count($primary, 'gist_intrapage_buffered'), '>', 0,
	'buffered build creates skip tuples');

$primary->safe_psql(
	'postgres', q[
INSERT INTO gist_intrapage
SELECT i AS id, point(i % 200, i / 200) AS p
FROM generate_series(40001, 50000) AS i;
]);
is(exact_result($primary, 'off'), exact_result($primary, 'on'),
	'inserting through skip groups preserves the exact scan result');
cmp_ok(marker_count($primary, 'gist_intrapage_buffered'), '>', 0,
	'inserting through skip groups preserves skip tuples');

$primary->safe_psql(
	'postgres', q[
DROP INDEX gist_intrapage_sorted;
DELETE FROM gist_intrapage WHERE id % 10 <> 0;
VACUUM gist_intrapage;
]);
is(exact_result($primary, 'off'), exact_result($primary, 'on'),
	'VACUUM and page deletion preserve the exact scan result');
cmp_ok(marker_count($primary, 'gist_intrapage_buffered'), '>', 0,
	'VACUUM page deletion preserves unaffected skip tuples');

$primary->wait_for_replay_catchup($standby);
is(marker_count($standby, 'gist_intrapage_buffered'),
	marker_count($primary, 'gist_intrapage_buffered'),
	'standby has the same skip tuples');
is(exact_result($standby, 'off'), exact_result($primary, 'on'),
	'standby replay preserves the exact scan result');

$primary->stop('immediate');
$primary->start;
is(exact_result($primary, 'off'), exact_result($primary, 'on'),
	'crash recovery preserves the exact scan result');

done_testing();
