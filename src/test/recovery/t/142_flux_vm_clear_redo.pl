# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A change to a page clears the page's visibility map bits and its
# PD_ALL_VISIBLE flag in the WAL record of the change, and replay of that
# record clears them too: heap flags the record XLH_*_ALL_VISIBLE_CLEARED.
# Otherwise a standby that has replayed the change, but not yet a later
# record that clears the bits, sees an all-visible page holding a version
# not every snapshot sees, and a page restored by crash recovery or replayed
# on a standby keeps PD_ALL_VISIBLE.  An index-only scan trusts the bit.
#
# For each kind of change, on heap (the control) and FLUX: the primary after
# the change, a standby stopped right after the change record (which is also
# what a crash right after that record leaves behind), the standby once caught
# up, and the primary after crash recovery.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "autovacuum = off\n");
$primary->start;

foreach my $ext ('pg_visibility', 'pg_walinspect')
{
	plan skip_all => "Extension $ext not installed"
	  unless $primary->check_extension($ext);
	$primary->safe_psql('postgres', "CREATE EXTENSION $ext");
}

my $copyfile = $primary->basedir . '/rows.tsv';
open(my $fh, '>', $copyfile) or die "could not write $copyfile: $!";
print $fh "11\t0\tx\n12\t0\tx\n";
close($fh);

# Each change touches block 0 of its own table and commits; %t is the table.
my @kinds = (
	[ 'insert', "INSERT INTO %t VALUES (11, 0, 'x')" ],
	[ 'copy', "COPY %t FROM '$copyfile'" ],
	[ 'spec', "INSERT INTO %t VALUES (11, 0, 'x') ON CONFLICT DO NOTHING" ],
	[ 'delete', "DELETE FROM %t WHERE id = 3" ],
	[ 'update', "UPDATE %t SET v = 1 WHERE id = 3" ],
	[ 'grow', "UPDATE %t SET pad = repeat('y', 200) WHERE id = 3" ],
	[ 'key', "UPDATE %t SET id = 100 WHERE id = 3" ],);

my @cases;
foreach my $am ('heap', 'flux')
{
	foreach my $k (@kinds)
	{
		my $t = "vm_$k->[0]_$am";
		$primary->safe_psql(
			'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, v int, pad text) USING $am;
INSERT INTO $t SELECT g, 0, 'x' FROM generate_series(1, 10) g;
VACUUM (FREEZE) $t;
});
		is( $primary->safe_psql(
				'postgres',
				"SELECT all_visible, pd_all_visible FROM pg_visibility('$t', 0)"
			),
			't|t',
			"$t: page is all-visible before the change");
		push @cases, { t => $t, sql => ($k->[1] =~ s/%t/$t/gr) };
	}
}

$primary->backup('bk');

# Make the changes, and find the record of each.
foreach my $c (@cases)
{
	my $t = $c->{t};
	my $before =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$primary->safe_psql('postgres', $c->{sql});
	my $after =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$c->{lsn} = $primary->safe_psql(
		'postgres', qq{
SELECT start_lsn FROM pg_get_wal_block_info('$before', '$after', false)
WHERE relfilenode = pg_relation_filenode('$t') AND relforknumber = 0
  AND relblocknumber = 0
  AND record_type NOT IN ('VM_SET', 'VM_CLEAR', 'FPI_FOR_HINT')
ORDER BY start_lsn LIMIT 1});
	ok($c->{lsn} ne '', "$t: found the change record");
	is( $primary->safe_psql(
			'postgres',
			"SELECT all_visible, pd_all_visible FROM pg_visibility('$t', 0)"),
		'f|f',
		"$t: primary: page not all-visible after the change");
}

# A standby that stops right after each change record.  The changes were
# made in order, so each restart moves the target forward.
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"recovery_target_inclusive = on\nrecovery_target_action = 'pause'\n");
foreach my $c (@cases)
{
	my $t = $c->{t};
	$standby->append_conf('postgresql.conf',
		"recovery_target_lsn = '$c->{lsn}'\n");
	$standby->start;
	$standby->poll_query_until('postgres',
		"SELECT pg_get_wal_replay_pause_state() = 'paused'")
	  or die "standby did not pause at $c->{lsn}";
	is( $standby->safe_psql(
			'postgres',
			"SELECT all_visible, pd_all_visible FROM pg_visibility('$t', 0)"),
		'f|f',
		"$t: standby stopped after the change record: page not all-visible");
	$standby->stop;
}

# The standby caught up.
$standby->append_conf('postgresql.conf', "recovery_target_lsn = ''\n");
$standby->start;
$primary->wait_for_catchup($standby);
foreach my $c (@cases)
{
	my $t = $c->{t};
	is( $standby->safe_psql(
			'postgres',
			"SELECT all_visible, pd_all_visible FROM pg_visibility('$t', 0)"),
		'f|f',
		"$t: standby caught up: page not all-visible");
}
$standby->stop;

# Crash recovery replays the changes from the full-page images they took.
$primary->stop('immediate');
$primary->start;
foreach my $c (@cases)
{
	my $t = $c->{t};
	is( $primary->safe_psql(
			'postgres',
			"SELECT all_visible, pd_all_visible FROM pg_visibility('$t', 0)"),
		'f|f',
		"$t: after crash recovery: page not all-visible");
}

done_testing();
