# Copyright (c) 2026, PostgreSQL Global Development Group
#
# An INSERT into a page whose free space holds stale bytes must replay to a
# page that wal_consistency_checking accepts.
#
# PageAddItem copies exactly the item's length and leaves the alignment
# padding up to the next MAXALIGN boundary as it was.  On the primary that
# is whatever an earlier tuple left there before VACUUM compacted the page;
# on a standby that restored the page from a full-page image, whose hole is
# zero-filled, it is zero.  The padding is not part of any tuple, so the
# masking function must ignore it, as heap_mask does.  Heap is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"autovacuum = off\nwal_consistency_checking = all\n");
$node->start;
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

foreach my $am (qw(heap flux))
{
	my $t = "t_$am";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, pad text) USING $am WITH (autovacuum_enabled = off);
INSERT INTO $t SELECT g, repeat('x', 200) FROM generate_series(1, 30) g;
DELETE FROM $t WHERE id % 3 <> 0;
});
	# VACUUM leaves LP_UNUSED slots and compacts the page; the stale 'x'
	# bytes of the deleted rows are now in the page's free space.
	$node->safe_psql('postgres', "VACUUM $t");
	# The next change to the page after a checkpoint carries a full-page
	# image, which a standby restores with a zeroed hole.
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', "INSERT INTO $t VALUES (100, 'a')");
	# These land in the old free space.  Lengths 1..8 make sure some items
	# are not MAXALIGNed and so are followed by padding.
	$node->safe_psql('postgres',
		"INSERT INTO $t SELECT 100 + g, repeat('y', g) FROM generate_series(1, 8) g"
	);

	# wait_for_catchup dies if the standby's startup process has exited.
	my $caught_up = eval { $node->wait_for_catchup($standby); 1 };
	ok($caught_up && $standby->is_alive,
		"$am: standby replays INSERT into a compacted page");
	last unless $caught_up && $standby->is_alive;
	is( $standby->safe_psql(
			'postgres', "SELECT count(*), sum(length(pad)) FROM $t"),
		$node->safe_psql(
			'postgres', "SELECT count(*), sum(length(pad)) FROM $t"),
		"$am: standby sees the primary's rows");
}

ok(!$standby->log_contains('inconsistent page found'),
	'standby found no inconsistent page');

$standby->stop('fast', fail_ok => 1);
$node->stop;
done_testing();
