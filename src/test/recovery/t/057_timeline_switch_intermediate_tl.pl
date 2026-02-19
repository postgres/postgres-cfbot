# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test the same rule across two switches in consecutive segments.  Recovery
# targeting TL3 must first wait for TL2's copy of the TL1->TL2 switch-point
# segment, and then for TL3's copy of the TL2->TL3 switch-point segment.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;
use File::Copy qw(copy);

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, has_archiving => 1);
$node_primary->start;
$node_primary->safe_psql('postgres', 'CREATE TABLE t (i int)');
$node_primary->backup('primary_backup');

# Put the first switch point in a new, partially filled segment.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (1)');

my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->init_from_backup($node_primary, 'primary_backup',
	has_streaming => 1);
$node_standby1->append_conf('postgresql.conf', "archive_mode = off");
$node_standby1->start;
$node_primary->wait_for_catchup($node_standby1);
$node_standby1->promote;
$node_standby1->poll_query_until('postgres',
	'SELECT NOT pg_is_in_recovery()')
  or die "Timed out waiting for promotion to TL2";

# Create and stage the correct TL2 copy of the first switch-point segment.
$node_standby1->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
my $tl2_lsn =
  $node_standby1->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
my $tl2_walfile = $node_standby1->safe_psql('postgres',
	"SELECT pg_walfile_name('$tl2_lsn'::pg_lsn)");
$node_standby1->safe_psql('postgres', 'SELECT pg_switch_wal()');

my $archive = $node_primary->archive_dir;
my $staged_tl2 = "$archive/$tl2_walfile.ready";
copy($node_standby1->data_dir . "/pg_wal/$tl2_walfile", $staged_tl2)
  or die "Could not stage $tl2_walfile: $!";

# Put the second switch point in the next segment.
$node_standby1->safe_psql('postgres', 'INSERT INTO t VALUES (3)');
$node_standby1->stop;
$node_standby1->backup_fs_cold('standby1_backup');
$node_standby1->start;

my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->init_from_backup($node_standby1, 'standby1_backup',
	has_streaming => 1);
$node_standby2->append_conf('postgresql.conf', "archive_mode = off");
$node_standby2->start;
$node_standby1->wait_for_catchup($node_standby2);
$node_standby2->promote;
$node_standby2->poll_query_until('postgres',
	'SELECT NOT pg_is_in_recovery()')
  or die "Timed out waiting for promotion to TL3";

# Create and stage the correct TL3 copy of the second switch-point segment.
$node_standby2->safe_psql('postgres', 'INSERT INTO t VALUES (4)');
my $target_lsn =
  $node_standby2->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
my $tl3_walfile = $node_standby2->safe_psql('postgres',
	"SELECT pg_walfile_name('$target_lsn'::pg_lsn)");
$node_standby2->safe_psql('postgres', 'SELECT pg_switch_wal()');

my $staged_tl3 = "$archive/$tl3_walfile.ready";
copy($node_standby2->data_dir . "/pg_wal/$tl3_walfile", $staged_tl3)
  or die "Could not stage $tl3_walfile: $!";

# Meanwhile, continue TL1 through both segments and archive its divergent
# copies.  They must never be used while recovering toward TL3.
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (-1)');
my $tl1_first_walfile = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (-2)');
my $tl1_last_walfile = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$tl1_last_walfile' FROM pg_stat_archiver")
  or die "Timed out waiting for divergent TL1 WAL to be archived";

# The TL3 history file contains the complete ancestry.  Publish it without
# publishing either correct switch-point segment yet.
copy($node_standby2->data_dir . '/pg_wal/00000003.history',
	"$archive/00000003.history")
  or die "Could not copy 00000003.history: $!";

ok(-f "$archive/$tl1_first_walfile",
	'parent copy of first switch-point segment is archived');
ok(-f "$archive/$tl1_last_walfile",
	'parent copy of second switch-point segment is archived');
ok(!-f "$archive/$tl2_walfile", 'TL2 switch-point segment is absent');
ok(!-f "$archive/$tl3_walfile", 'TL3 switch-point segment is absent');
is(substr($tl1_first_walfile, 8), substr($tl2_walfile, 8),
	'first parent and child files have the same segment number');
is(substr($tl1_last_walfile, 8), substr($tl3_walfile, 8),
	'second parent and child files have the same segment number');

$node_primary->stop;
$node_standby1->stop;
$node_standby2->stop;

my $node_rec = PostgreSQL::Test::Cluster->new('recovering');
$node_rec->init_from_backup($node_primary, 'primary_backup',
	has_restoring => 1);
$node_rec->enable_restoring($node_primary, 1);
$node_rec->append_conf('postgresql.conf', <<EOM);
recovery_target_timeline = '3'
recovery_target_lsn = '$target_lsn'
recovery_target_action = 'promote'
wal_retrieve_retry_interval = '100ms'
log_min_messages = debug1
EOM
$node_rec->start;

# Recovery first reaches the TL1->TL2 switch-point segment.  It must wait for
# TL2 rather than fall through to the archived TL1 copy.
$node_rec->wait_for_log(
	qr/not searching older timelines for WAL segment "\Q$tl2_walfile\E"/);
copy($staged_tl2, "$archive/$tl2_walfile")
  or die "Could not publish $tl2_walfile: $!";

# In the following segment TL3 is now the newest eligible timeline.  Recovery
# must again wait rather than use an older copy.
$node_rec->wait_for_log(
	qr/not searching older timelines for WAL segment "\Q$tl3_walfile\E"/);
copy($staged_tl3, "$archive/$tl3_walfile")
  or die "Could not publish $tl3_walfile: $!";

$node_rec->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out waiting for recovery to reach TL3 target";
is($node_rec->safe_psql('postgres',
	q{SELECT string_agg(i::text, ',' ORDER BY i) FROM t}),
	'1,2,3,4', 'recovery followed the complete TL1-TL2-TL3 history');

done_testing();
