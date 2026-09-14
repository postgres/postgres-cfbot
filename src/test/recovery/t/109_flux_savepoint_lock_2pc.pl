# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A row lock taken in a savepoint that is rolled back leaves nothing behind,
# however the transaction ends: COMMIT, PREPARE TRANSACTION and COMMIT
# PREPARED, or ROLLBACK PREPARED.  Another session updates the row and
# commits after the savepoint rollback.  isolation/flux_savepoint_lock_
# rollback runs more variants without two-phase commit.  Heap is the control.
#
# FLUX used to mark the other session's committed image DELETED when the
# locker committed or prepared, on the primary only and with no WAL: a plain
# SELECT still returned the row, but UPDATE, DELETE and row locks did not
# find it, and the standby had a different page.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_consistency_checking = all
max_prepared_transactions = 5
));
$node->start;
if (   !$node->check_extension('pageinspect')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'pageinspect or amcheck not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION pageinspect; CREATE EXTENSION amcheck;');
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

# The FLUX_TUPLE_DELETED bit (0x0004 of t_flags, at byte 32) of row 1.
sub deleted_bit
{
	my ($n, $t) = @_;
	return $n->safe_psql(
		'postgres', qq{
SELECT get_byte(get_raw_page('$t', 0), lp_off + 32) & 4
  FROM heap_page_items(get_raw_page('$t', 0))
 WHERE lp = (SELECT (ctid::text::point)[1]::int FROM $t WHERE id = 1)});
}

my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $n = 0;
my @tables;
foreach my $am ('heap', 'flux')
{
	foreach my $mode ('FOR UPDATE', 'FOR NO KEY UPDATE', 'FOR SHARE',
		'FOR KEY SHARE')
	{
		foreach my $end ('COMMIT', 'COMMIT PREPARED', 'ROLLBACK PREPARED')
		{
			$n++;
			my $t = "t_${am}_$n";
			my $what = "$am, $mode, $end";
			$node->safe_psql(
				'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, v int) USING $am;
INSERT INTO $t VALUES (1, 0), (2, 0);
VACUUM $t;
});
			$s1->query_safe(
				"BEGIN; SAVEPOINT a; SELECT v FROM $t WHERE id = 1 $mode; ROLLBACK TO a;"
			);
			$node->safe_psql('postgres',
				"UPDATE $t SET v = v + 1 WHERE id = 1");
			if ($end eq 'COMMIT')
			{
				$s1->query_safe('COMMIT');
			}
			else
			{
				$s1->query_safe("PREPARE TRANSACTION 'p$n'");
				$node->safe_psql('postgres', "$end 'p$n'");
			}

			is( $node->safe_psql('postgres', "SELECT v FROM $t WHERE id = 1"),
				'1',
				"$what: SELECT finds the row");
			is( $node->safe_psql(
					'postgres', "SELECT v FROM $t WHERE id = 1 FOR KEY SHARE"
				),
				'1',
				"$what: FOR KEY SHARE finds the row");
			is( $node->safe_psql(
					'postgres', "SELECT v FROM $t WHERE id = 1 FOR UPDATE"),
				'1',
				"$what: FOR UPDATE finds the row");
			is( $node->safe_psql(
					'postgres', qq{
BEGIN;
UPDATE $t SET v = v + 100 WHERE id = 1;
DELETE FROM $t WHERE id = 1 RETURNING v;
ROLLBACK;}),
				'101',
				"$what: UPDATE, then DELETE of its new version");
			is( $node->safe_psql(
					'postgres', "SELECT bt_index_check('${t}_pkey', true)"),
				'',
				"$what: amcheck finds every row in the index");
			is(deleted_bit($node, $t), '0', "$what: row 1 is not DELETED")
			  if $am eq 'flux';
			push @tables, [ $t, $what, $am ];
		}
	}
}
$s1->quit;

$node->wait_for_catchup($standby);
foreach my $c (@tables)
{
	my ($t, $what, $am) = @$c;
	is( $standby->safe_psql(
			'postgres',
			"SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM $t"),
		'1:1,2:0',
		"$what: the standby has the rows");
	is( deleted_bit($standby, $t),
		deleted_bit($node, $t),
		"$what: the standby's row 1 has the primary's DELETED bit"
	) if $am eq 'flux';
}
ok($standby->is_alive, 'standby is alive');
ok(!$standby->log_contains('inconsistent page found'),
	'standby found no inconsistent page');

$standby->stop('fast', fail_ok => 1);
$node->stop;
done_testing();
