# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A change to an all-visible page must clear the page's visibility map bits
# even if the map was created after the changing backend last looked at its
# size.  A backend processes the invalidation that announces the new map
# only when it next takes a new lock, so one statement can look at the map
# (it has none), wait while VACUUM creates it and makes pages all-visible,
# and then change those pages.  The statement is a plpgsql function whose
# plans are already cached, and it waits on an advisory lock, so it takes
# no relation lock in between (a subtransaction that ends would release
# its lock, and the next statement's would process the invalidation).  heap is the control; it pins the map with
# extension, which reads the size again.
#
# A bit left set wrongly would let a VACUUM that skips all-frozen pages
# leave the change's xid behind relfrozenxid; that check is a guard.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_vm_stale_size');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_visibility');

foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE t_$am (id int, v int) USING $am;
INSERT INTO t_$am SELECT g, g FROM generate_series(1, 2000) g;
CREATE FUNCTION change_$am(t1 tid, t2 tid) RETURNS void LANGUAGE plpgsql AS \$\$
BEGIN
	UPDATE t_$am SET v = v + 1 WHERE ctid = '(0,1)';
	PERFORM pg_advisory_lock(1);
	UPDATE t_$am SET v = -1 WHERE ctid = t1;
	DELETE FROM t_$am WHERE ctid = t2;
	PERFORM pg_advisory_unlock(1);
END \$\$;
});
	my %id;
	foreach my $blk (1, 2)
	{
		$id{$blk} = $node->safe_psql('postgres',
			"SELECT id FROM t_$am WHERE ctid = '($blk,1)'");
	}
	is( $node->safe_psql(
			'postgres', "SELECT pg_relation_size('t_$am', 'vm')"),
		'0',
		"$am: no visibility map yet");

	# Cache the function's plans first.
	my $w = $node->background_psql('postgres');
	$w->query_safe("BEGIN; SELECT change_$am('(1,2)', '(2,2)'); ROLLBACK");

	my $holder = $node->background_psql('postgres');
	$holder->query_safe('SELECT pg_advisory_lock(1)');
	$w->query_safe('BEGIN');
	$w->query_until(qr/started/,
		"\\echo started\nSELECT change_$am('(1,1)', '(2,1)');\n");
	$node->poll_query_until('postgres',
		"SELECT count(*) > 0 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"
	) or die "the change did not wait";
	$node->safe_psql('postgres', "VACUUM (FREEZE) t_$am");
	is( $node->safe_psql(
			'postgres',
			"SELECT all_visible, all_frozen, pd_all_visible FROM pg_visibility('t_$am', 1)"
		),
		't|t|t',
		"$am: VACUUM made block 1 all-frozen");
	$holder->query_safe('SELECT pg_advisory_unlock(1)');
	$holder->quit;
	$w->query_safe('SELECT 1');

	foreach my $blk (1, 2)
	{
		is( $node->safe_psql(
				'postgres',
				"SELECT all_visible, all_frozen, pd_all_visible FROM pg_visibility('t_$am', $blk)"
			),
			'f|f|f',
			"$am: change in progress cleared block $blk");
	}
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*), sum(v) FROM t_$am WHERE id IN ($id{1}, $id{2})"
		),
		"2|" . ($id{1} + $id{2}),
		"$am: a seqscan does not see the changes in progress");
	$w->query_safe('COMMIT');
	$w->quit;
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*), sum(v) FROM t_$am WHERE id IN ($id{1}, $id{2})"
		),
		'1|-1',
		"$am: a seqscan sees the committed changes");

	# A VACUUM that skips all-frozen pages must not skip the page with the
	# UPDATE, or relfrozenxid passes its xid.
	my ($xmin) = $node->safe_psql('postgres',
		"SELECT xmin FROM t_$am WHERE id = $id{1}");
	$node->safe_psql('postgres', "VACUUM t_$am");
	is( $node->safe_psql(
			'postgres', qq{
SELECT age('$xmin'::xid) <= age(relfrozenxid) FROM pg_class
 WHERE relname = 't_$am'}),
		't',
		"$am: relfrozenxid does not pass the xid of the UPDATE");
	$node->safe_psql('postgres', "VACUUM (FREEZE) t_$am");
	is( $node->safe_psql(
			'postgres',
			"SELECT all_visible, all_frozen FROM pg_visibility('t_$am', 1)"),
		't|t',
		"$am: VACUUM froze block 1 again");
}

$node->stop;
done_testing();
