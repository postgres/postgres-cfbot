# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A table AM's DELETE called with wait = false (REPACK (CONCURRENTLY)
# applies its deletes that way) must neither wait for nor misreport a
# writer or locker still in progress: heap_delete returns TM_BeingModified
# with that transaction in tmfd->xmax, as tableam.h documents.  heap is the
# control for every case.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_delete_nowait');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

foreach my $am ('heap', 'flux')
{
	foreach my $case (
		[ 'DELETE', 'DELETE FROM t_AM WHERE id = 1' ],
		[ 'UPDATE', 'UPDATE t_AM SET v = v + 1 WHERE id = 1' ],
		[ 'FOR UPDATE', 'SELECT * FROM t_AM WHERE id = 1 FOR UPDATE' ],
		[ 'FOR SHARE', 'SELECT * FROM t_AM WHERE id = 1 FOR SHARE' ])
	{
		my ($what, $sql) = @$case;
		$sql =~ s/_AM/_$am/g;
		$node->safe_psql(
			'postgres', qq{
DROP TABLE IF EXISTS t_$am;
CREATE TABLE t_$am (id int, v int) USING $am;
INSERT INTO t_$am VALUES (1, 10), (2, 20);
});
		my $tid =
		  $node->safe_psql('postgres', "SELECT ctid FROM t_$am WHERE id = 1");
		my $s1 = $node->background_psql('postgres');
		$s1->query_safe('BEGIN');
		my $xid = $s1->query_safe('SELECT txid_current()');
		$s1->query_safe($sql);
		is( $node->safe_psql(
				'postgres', "SELECT idxundo_delete_nowait('t_$am', '$tid')"),
			"BeingModified $xid",
			"$am: delete without waiting, $what in progress");
		$s1->query_safe('ROLLBACK');
		$s1->quit;
		is( $node->safe_psql('postgres', "SELECT v FROM t_$am WHERE id = 1"),
			'10',
			"$am: delete without waiting deleted nothing ($what)");
	}

	# Nothing in progress: the delete is done.
	my $tid =
	  $node->safe_psql('postgres', "SELECT ctid FROM t_$am WHERE id = 1");
	is( $node->safe_psql(
			'postgres', "SELECT idxundo_delete_nowait('t_$am', '$tid')"),
		'Ok',
		"$am: delete without waiting, no conflict");
	is($node->safe_psql('postgres', "SELECT count(*) FROM t_$am"),
		'1', "$am: deleted");
}

$node->stop;
done_testing();
