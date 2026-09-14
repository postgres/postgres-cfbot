
# Copyright (c) 2026, PostgreSQL Global Development Group

# VACUUM FULL and CLUSTER of a FLUX table must leave valid relfrozenxid /
# relminmxid behind, on the table and on its TOAST table, so that a later
# VACUUM (which asserts them normal) and anti-wraparound bookkeeping work.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_rewrite');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE vf (id int PRIMARY KEY, v text) USING flux;
INSERT INTO vf SELECT g, repeat('x', 3000 + g) FROM generate_series(1, 50) g;
DELETE FROM vf WHERE id % 2 = 0;
});

my $cutoffs = q{
SELECT c.relfrozenxid::text::bigint > 2, c.relminmxid::text::bigint >= 1,
       t.relfrozenxid::text::bigint > 2, t.relminmxid::text::bigint >= 1
FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid
WHERE c.relname = 'vf'};

foreach my $cmd ('VACUUM FULL vf', 'CLUSTER vf USING vf_pkey')
{
	$node->safe_psql('postgres', $cmd);
	is($node->safe_psql('postgres', $cutoffs),
		't|t|t|t', "$cmd: valid cutoffs");
	$node->safe_psql('postgres', 'VACUUM vf');
	is( $node->safe_psql(
			'postgres', q{SELECT count(*), sum(length(v)) FROM vf}),
		'25|75625',
		"$cmd: then VACUUM: rows intact");
}

# FREEZE and a database-wide VACUUM (advances datfrozenxid) work too.
$node->safe_psql('postgres', 'VACUUM (FREEZE) vf');
$node->safe_psql('postgres', 'VACUUM');
is($node->safe_psql('postgres', 'SELECT count(*) FROM vf'),
	'25', 'rows after FREEZE');

$node->stop;
done_testing();
