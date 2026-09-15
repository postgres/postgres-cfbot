# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Check a table whose TOAST values have oid8 IDs past 2^32, on the heap
# side and on the TOAST side.  The amcheck regression tests use a young
# OID counter, so their oid8 values have IDs that fit in four bytes.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $next_oid = '4295067296';    # 2^32 + 100000

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init;
command_ok(
	[ 'pg_resetwal', '--next-oid' => $next_oid, $node->data_dir ],
	'set an 8-byte OID counter');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');

# Out-of-line values, uncompressed and compressed, some of them replaced
# or deleted so that the TOAST table holds dead chunks as well.  The
# compressed values are large enough to stay out of line once compressed.
$node->safe_psql(
	'postgres', q(
CREATE TABLE toast_oid8 (a int PRIMARY KEY, b text)
  WITH (toast_value_type = 'oid8');
ALTER TABLE toast_oid8 ALTER COLUMN b SET STORAGE EXTERNAL;
INSERT INTO toast_oid8
  SELECT gs, repeat(md5(gs::text), 400) FROM generate_series(1, 5) gs;
ALTER TABLE toast_oid8 ALTER COLUMN b SET STORAGE EXTENDED;
INSERT INTO toast_oid8
  SELECT gs, repeat(md5(gs::text), 20000) FROM generate_series(6, 10) gs;
UPDATE toast_oid8 SET b = repeat(md5('plugh'), 20000) WHERE a IN (3, 8);
DELETE FROM toast_oid8 WHERE a IN (5, 10);
));

my $toast_table = $node->safe_psql('postgres',
	"SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toast_oid8'"
);
my $toast_index = $node->safe_psql('postgres',
	"SELECT indexrelid::regclass FROM pg_index WHERE indrelid = '$toast_table'::regclass"
);

is( $node->safe_psql(
		'postgres',
		"SELECT count(*), count(pg_column_toast_chunk_id(b)),
		        min(pg_column_toast_chunk_id(b)) > '$next_oid'::oid8,
		        count(*) FILTER (WHERE pg_column_compression(b) IS NOT NULL)
		   FROM toast_oid8"),
	'8|8|t|5',
	'all values are out of line, with IDs past 2^32');

sub check_all
{
	my ($stage) = @_;

	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM verify_heapam('toast_oid8', check_toast := true)"
		),
		'0',
		"verify_heapam reports nothing for the table and its TOAST values $stage"
	);
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM verify_heapam('$toast_table')"),
		'0',
		"verify_heapam reports nothing for the TOAST table $stage");
	is( $node->safe_psql(
			'postgres',
			"SELECT bt_index_parent_check('$toast_index', heapallindexed => true)"
		),
		'',
		"bt_index_parent_check passes on the TOAST index $stage");
}

check_all('with dead chunks');
$node->safe_psql('postgres', 'VACUUM toast_oid8');
check_all('after VACUUM');

$node->stop;

done_testing();
