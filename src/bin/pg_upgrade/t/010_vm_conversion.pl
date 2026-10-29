# Copyright (c) 2026, PostgreSQL Global Development Group

# The visibility map widened from 2 to 4 bits per heap block.  Upgrading
# across that requires rewriting every _vm fork, and because the fork
# doubles in length the rewrite must re-segment it.  Swap mode moves the old
# files into place and so cannot rewrite them; it must refuse such an
# upgrade before touching the old cluster.
#
# To run, set 'oldinstall' ENV variable to point to an installation from
# before the widening.  Without it, there is nothing to convert and the test
# is skipped.  With a small RELSEG_SIZE (--with-segsize-blocks), the map
# spans several segments and the re-segmentation is exercised too; the old
# and new installations must use the same segment size.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!defined $ENV{oldinstall})
{
	plan skip_all => 'test requires oldinstall';
}

my $old = PostgreSQL::Test::Cluster->new('old_node',
	install_path => $ENV{oldinstall});
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Passing options to init() makes it run the old initdb rather than copy the
# new cluster's template.  Checksums are enabled so that the rewritten pages'
# checksums are verified when the new cluster reads them.
$old->init(extra => ['--data-checksums']);

# pg_upgrade decides whether to rewrite the map from the old cluster's catalog
# version, so skip on the same basis.  This must match
# VISIBILITYMAP_WIDTH_CHANGE_CAT_VER in pg_upgrade.h.
my $width_change_cat_ver = 202610201;
my ($controldata) =
  run_command([ $old->installed_command('pg_controldata'), $old->data_dir ]);
my ($old_cat_ver) = $controldata =~ /^Catalog version number:\s+(\d+)$/m;
defined $old_cat_ver
  or die "could not read catalog version from pg_controldata";
if ($old_cat_ver >= $width_change_cat_ver)
{
	plan skip_all => 'old cluster already has the 4-bit visibility map';
}
$old->append_conf(
	'postgresql.conf', q{
autovacuum = off
checkpoint_timeout = '1h'
max_wal_size = '20GB'
});
$old->start;

my ($block_size, $segment_blocks) = split /\|/, $old->safe_psql(
	'postgres',
	q{SELECT current_setting('block_size'), setting
	  FROM pg_settings WHERE name = 'segment_size'});

# Heap blocks described by one old-format map page: four per byte of the
# page's map area, which is the page minus its 24-byte header (MAPSIZE in
# visibilitymap.c).
my $old_blocks_per_map_page = ($block_size - 24) * 4;

# Make the old map one page longer than a segment when segments are small, so
# that both the old and the new fork have several segments.  Otherwise, just
# use two old map pages.
my $old_map_pages = $segment_blocks < 1000 ? $segment_blocks + 1 : 2;
my $nblocks = ($old_map_pages - 1) * $old_blocks_per_map_page + 1000;
my $tail = $nblocks - 1000;

# One row per heap block.  All blocks are frozen, then a tail of blocks is
# added that VACUUM marks all-visible without freezing, and finally a sparse
# set of rows is deleted to clear both bits on some blocks.  That leaves every
# combination of the two bits, at every position within a map byte.
$old->safe_psql(
	'postgres', qq{
	CREATE EXTENSION pg_visibility;
	CREATE TABLE vm (id int, pad text) WITH (fillfactor = 10);
	INSERT INTO vm SELECT g, repeat('x', 600) FROM generate_series(1, $tail) g;
	VACUUM (FREEZE) vm;
	CHECKPOINT;
	INSERT INTO vm SELECT g, repeat('x', 600)
	  FROM generate_series($tail + 1, $nblocks) g;
	VACUUM vm;
	DELETE FROM vm WHERE id % 101 = 0;
});

my $map_query = q{
	SELECT count(*), md5(string_agg(blkno || ':' || all_visible || all_frozen,
								',' ORDER BY blkno))
	FROM pg_visibility_map('vm')};
my $summary_query = q{
	SELECT count(*) FILTER (WHERE all_visible AND all_frozen),
		   count(*) FILTER (WHERE all_visible AND NOT all_frozen),
		   count(*) FILTER (WHERE NOT all_visible AND NOT all_frozen)
	FROM pg_visibility_map('vm')};

my $expect_map = $old->safe_psql('postgres', $map_query);
my $summary = $old->safe_psql('postgres', $summary_query);
my ($both, $visible, $neither) = split /\|/, $summary;
cmp_ok($both, '>', 0, 'old map has all-frozen blocks');
cmp_ok($visible, '>', 0, 'old map has all-visible, not all-frozen blocks');
cmp_ok($neither, '>', 0, 'old map has blocks with neither bit');
my $old_vm_size =
  $old->safe_psql('postgres', q{SELECT pg_relation_size('vm', 'vm')});
is($old_vm_size, $old_map_pages * $block_size, 'old map has expected size');
my $expect_data =
  $old->safe_psql('postgres', q{SELECT count(*), sum(id) FROM vm});
$old->stop;

my $new = PostgreSQL::Test::Cluster->new('new_node');
$new->init(extra => ['--data-checksums']);

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

my @upgrade = (
	'pg_upgrade', '--no-sync',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port);

# Swap mode must refuse, before anything is transferred.
command_checks_all(
	[ @upgrade, '--swap', '--check' ],
	1,
	[
		qr/Swap mode cannot upgrade clusters whose visibility maps must be rewritten|could not .* between old and new data directories: .*/
	],
	[qr//],
	'swap mode refuses a visibility map rewrite');
command_ok([ @upgrade, $mode ], 'pg_upgrade rewrites the visibility map');

$new->start;

is($new->safe_psql('postgres', q{SELECT count(*), sum(id) FROM vm}),
	$expect_data, 'table contents preserved');
# Reading the map verifies each page's checksum, so this also checks that
# they were computed for the pages' new locations.
is($new->safe_psql('postgres', $map_query),
	$expect_map, 'visibility map bits preserved for every block');
is( $new->safe_psql('postgres', q{SELECT pg_relation_size('vm', 'vm')}),
	2 * $old_vm_size,
	'new map is twice as long');
is( $new->safe_psql(
		'postgres', q{
		SELECT count(*) FROM pg_check_visible('vm')
		UNION ALL SELECT count(*) FROM pg_check_frozen('vm')}),
	"0\n0",
	'no visible or frozen bit disagrees with the heap');

# Every segment of the new map is full, except the last.
my $vm_path = $new->data_dir . '/'
  . $new->safe_psql('postgres', q{SELECT pg_relation_filepath('vm')}) . '_vm';
my @sizes;
for (my $segno = 0;; $segno++)
{
	my $file = $segno == 0 ? $vm_path : "$vm_path.$segno";
	last unless -e $file;
	push @sizes, -s $file;
}
my $new_pages = 2 * $old_map_pages;
my @expect_sizes;
for (my $p = $new_pages; $p > 0; $p -= $segment_blocks)
{
	push @expect_sizes,
	  ($p < $segment_blocks ? $p : $segment_blocks) * $block_size;
}
is("@sizes", "@expect_sizes", 'new map is correctly segmented');

# The upgraded map remains usable: VACUUM agrees with it, and a full VACUUM
# FREEZE marks everything.
$new->safe_psql('postgres', 'VACUUM (FREEZE) vm');
is( $new->safe_psql(
		'postgres',
		q{SELECT count(*) FILTER (WHERE NOT all_frozen) FROM pg_visibility_map('vm')}
	),
	'0',
	'VACUUM FREEZE on the new cluster sets every block all-frozen');

$new->stop;

done_testing();
