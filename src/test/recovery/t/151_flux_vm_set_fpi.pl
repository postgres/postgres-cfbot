# Copyright (c) 2026, PostgreSQL Global Development Group
#
# VACUUM's visibility map changes must survive crash recovery and replay.
# The first change to a map page after a checkpoint is logged with a
# full-page image.  The map lies between pd_lower and pd_upper, the hole a
# standard page image leaves out, so a map page registered as standard is
# restored without its bits.  Heap registers map pages as non-standard.
#
# For each AM: a checkpoint, then VACUUM (FREEZE) sets the bits of a new
# map (its first change since the checkpoint), compared on a standby; then
# a change clears one page's bits, another checkpoint, VACUUM (FREEZE) sets
# them again, compared on the standby and after crash recovery.  heap is
# the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "autovacuum = off\n");
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION pg_visibility; CREATE TABLE flushed (i int)');
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

sub bits
{
	my ($node, $t) = @_;
	return $node->safe_psql(
		'postgres', qq{
SELECT count(*) || ' pages, ' || count(*) FILTER (WHERE all_visible)
  || ' all-visible, ' || count(*) FILTER (WHERE all_frozen) || ' all-frozen'
  FROM pg_visibility_map('$t')});
}

# A VACUUM's WAL is not flushed at its end; a committed write flushes it.
sub flush_wal
{
	$primary->safe_psql('postgres', 'INSERT INTO flushed VALUES (1)');
}

my %all;
foreach my $am ('heap', 'flux')
{
	my $t = "vs_$am";
	$primary->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, pad text) USING $am;
INSERT INTO $t SELECT g, repeat('x', 100) FROM generate_series(1, 150) g;
});
	$primary->safe_psql('postgres', 'CHECKPOINT');
	$primary->safe_psql('postgres', "VACUUM (FREEZE) $t");
	$all{$am} = bits($primary, $t);
	like(
		$all{$am},
		qr/^(\d+) pages, \1 all-visible, \1 all-frozen$/,
		"$am: VACUUM set the bits: $all{$am}");
	flush_wal();
	$primary->wait_for_replay_catchup($standby);
	is(bits($standby, $t), $all{$am}, "$am: standby replayed the new map");

	# A change clears block 1; after a checkpoint, VACUUM sets it again.
	$primary->safe_psql('postgres', "DELETE FROM $t WHERE id = 80");
	$primary->safe_psql('postgres', 'CHECKPOINT');
	$primary->safe_psql('postgres', "VACUUM (FREEZE) $t");
	is(bits($primary, $t), $all{$am}, "$am: VACUUM set the bits again");
}
flush_wal();
$primary->wait_for_replay_catchup($standby);
foreach my $am ('heap', 'flux')
{
	is(bits($standby, "vs_$am"),
		$all{$am}, "$am: standby replayed the map after a checkpoint");
}

$primary->stop('immediate');
$primary->start;
foreach my $am ('heap', 'flux')
{
	is(bits($primary, "vs_$am"),
		$all{$am}, "$am: crash recovery keeps the bits");
}

$standby->stop;
$primary->stop;
done_testing();
