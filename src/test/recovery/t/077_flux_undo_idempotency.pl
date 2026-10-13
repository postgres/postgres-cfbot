# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX UNDO apply must be idempotent: a record whose change was already
# undone (by ROLLBACK TO SAVEPOINT, or by a writer that reverted it) must not
# be applied again on top of a version some other transaction committed
# since.  flux_undo_apply checks that the tuple still carries the version the
# record produced (t_verptr == the record's pointer for UPDATE, or a DELETE
# mark by the record's writer) and otherwise leaves the tuple alone.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_idem');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE f (id int PRIMARY KEY, v int) USING flux;
INSERT INTO f SELECT g, g * 10 FROM generate_series(1, 6) g;
});

# T1 changes r2 (UPDATE) and r5 (DELETE) in a savepoint and rolls the
# savepoint back; T2 then commits its own change to both rows; T1 writes
# again and rolls back.  The top-level rollback walks every record T1 wrote,
# including the ones ROLLBACK TO SAVEPOINT already undid.
my $t1 = $node->background_psql('postgres');
$t1->query_safe('BEGIN');
$t1->query_safe('UPDATE f SET v = 11 WHERE id = 1');
$t1->query_safe('SAVEPOINT s');
$t1->query_safe('UPDATE f SET v = 22 WHERE id = 2');
$t1->query_safe('DELETE FROM f WHERE id = 5');
$t1->query_safe('ROLLBACK TO s');

$node->safe_psql('postgres', 'UPDATE f SET v = 200 WHERE id = 2');
$node->safe_psql('postgres', 'UPDATE f SET v = 500 WHERE id = 5');

# Write again at the top level, so the top-level chain starts after the
# savepoint's records.
$t1->query_safe('RELEASE s');
$t1->query_safe('UPDATE f SET v = 33 WHERE id = 3');
$t1->query_safe('ROLLBACK');
$t1->quit;

is( $node->safe_psql(
		'postgres',
		'SELECT string_agg(id || \':\' || v, \',\' ORDER BY id) FROM f'),
	'1:10,2:200,3:30,4:40,5:500,6:60',
	'top-level rollback did not re-apply records undone by ROLLBACK TO');

$node->stop;
done_testing();
