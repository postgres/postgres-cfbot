
# Copyright (c) 2026, PostgreSQL Global Development Group

# FLUX in-place UPDATE must not leak superseded TOAST values.
#
# An in-place UPDATE overwrites the row at its TID and keeps the old version
# only as an UNDO before-image.  The old version's external TOAST values must
# then be deleted exactly like heap deletes them: by the updating transaction
# (so they survive for snapshot-toast readers of the before-image, and an
# abort keeps them), and physically reclaimed by VACUUM once no snapshot can
# reach the before-image.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_toast');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE t (id int PRIMARY KEY, n int, big text) USING flux;
ALTER TABLE t ALTER big SET STORAGE EXTERNAL;
INSERT INTO t VALUES (1, 0, repeat('a', 10000)), (2, 0, repeat('z', 10000));
});

# The number of live wide values (one per row whose wide column is set).  On
# this non-logically-logged FLUX relation wide values live in FLUX-native
# overflow, not the heap TOAST table, so a heap-TOAST chunk count would always
# be 0; count the rows that hold a wide value instead -- an AM-agnostic live
# count.  Physical reclaim of superseded native-overflow chunks (no leak, no
# corruption, before-image MVCC, standby/crash replay) is covered by
# recovery/162_flux_overflow_vacuum_reclaim.
my $values = "SELECT count(*) FILTER (WHERE big IS NOT NULL) FROM t";
# After VACUUM every live wide value must still read back whole (10000 bytes):
# a VACUUM that dropped or truncated a live native-overflow value would fail
# this, AM-agnostically.
my $physical = qq{
SELECT bool_and(octet_length(big) = 10000) FROM t WHERE big IS NOT NULL};

# Five UPDATEs of the wide column, five of a narrow column.
for my $i (1 .. 5)
{
	$node->safe_psql('postgres',
		"UPDATE t SET big = repeat(chr(97 + $i), 10000) WHERE id = 1");
	$node->safe_psql('postgres', "UPDATE t SET n = n + 1 WHERE id = 1");
}
is($node->safe_psql('postgres', $values),
	'2', 'superseded TOAST values are deleted by the UPDATE');
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $physical),
	't', 'VACUUM reclaimed the superseded TOAST chunks');
is( $node->safe_psql(
		'postgres',
		'SELECT n, big = repeat(\'f\', 10000) FROM t WHERE id = 1'),
	'5|t',
	'current value intact');

# A REPEATABLE READ snapshot taken before the UPDATE still detoasts the old
# value through the before-image, even across a VACUUM.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$bg->query_safe('SELECT count(*) FROM t');
$node->safe_psql('postgres',
	"UPDATE t SET big = repeat('g', 10000) WHERE id = 1");
$node->safe_psql('postgres', 'VACUUM t');
is($bg->query_safe("SELECT big = repeat('f', 10000) FROM t WHERE id = 1"),
	't', 'old snapshot detoasts the superseded value');
$bg->query_safe('COMMIT');
$bg->quit;
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $physical),
	't', 'superseded value reclaimed once the old snapshot ended');
is($node->safe_psql('postgres', $values), '2', 'two live values remain');

# An aborted UPDATE (top level and savepoint) restores a detoastable value.
$node->safe_psql(
	'postgres', q{
BEGIN; UPDATE t SET big = repeat('h', 10000) WHERE id = 1; ROLLBACK;
BEGIN; SAVEPOINT s; UPDATE t SET big = repeat('i', 10000) WHERE id = 1;
ROLLBACK TO s; COMMIT;
VACUUM t;
});
is( $node->safe_psql(
		'postgres', "SELECT big = repeat('g', 10000) FROM t WHERE id = 1"),
	't',
	'aborted UPDATE keeps the restored value detoastable');
is($node->safe_psql('postgres', $values),
	'2', 'aborted UPDATEs left no values');
is($node->safe_psql('postgres', $physical),
	't', 'aborted UPDATEs left no chunks after VACUUM');

# Taking another row's TOAST pointer must copy the value, not share it.
$node->safe_psql(
	'postgres', q{
UPDATE t SET big = (SELECT big FROM t WHERE id = 2) WHERE id = 1;
DELETE FROM t WHERE id = 2;
VACUUM t;
});
is( $node->safe_psql(
		'postgres', "SELECT big = repeat('z', 10000) FROM t WHERE id = 1"),
	't',
	'value copied from another row survives that row\'s removal');
is($node->safe_psql('postgres', $values), '1', 'one live value remains');

# A row that grows past its full page after toasting falls back to an
# out-of-place UPDATE; the abandoned in-place attempt must not leave the
# value it externalized behind.
$node->safe_psql(
	'postgres', q{
CREATE TABLE g (id int, a text, b text) USING flux;
ALTER TABLE g ALTER a SET STORAGE EXTERNAL, ALTER b SET STORAGE EXTERNAL;
INSERT INTO g SELECT i, '', repeat('p', 150) FROM generate_series(1, 200) i;
UPDATE g SET a = repeat('q', 5000), b = repeat('r', 1500) WHERE id = 1;
});
is( $node->safe_psql(
		'postgres',
		"SELECT ctid <> '(0,1)', a = repeat('q', 5000) FROM g WHERE id = 1"),
	't|t',
	'row moved out of place');
# The abandoned in-place attempt must not leave an extra externalized value.
# On this non-logically-logged FLUX relation the value lives in FLUX-native
# overflow, not the heap TOAST table; VACUUM reclaims the orphaned chunks the
# abandoned attempt wrote (see recovery/162).  After VACUUM exactly one live
# wide value survives -- the committed out-of-place `a`.
$node->safe_psql('postgres', 'VACUUM g');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FILTER (WHERE octet_length(a) = 5000) FROM g"),
	'1',
	'abandoned in-place attempt left no extra wide value');

$node->stop;
done_testing();
