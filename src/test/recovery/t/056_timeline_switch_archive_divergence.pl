# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test that archive recovery does not use a parent timeline's WAL file for
# the segment containing a switch point when the child's file is absent.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;
use File::Copy qw(copy);

# Create TL1 and take a base backup before the segment containing the future
# switch point.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, has_archiving => 1);
$node_primary->start;
$node_primary->safe_psql('postgres', 'CREATE TABLE t (i int)');
$node_primary->backup('backup');

# Create a standby that can be promoted to TL2.  It must not archive its WAL,
# since the test initially needs the TL2 switch-point segment to be absent.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'backup', has_streaming => 1);
$node_standby->append_conf('postgresql.conf', "archive_mode = off");
$node_standby->start;

# Put the TL1->TL2 switch point in a new, partially filled segment.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (1)');
$node_primary->wait_for_catchup($node_standby);
$node_standby->promote;
$node_standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out waiting for promotion";

# Generate valid WAL on TL2, remember a recovery target after its commit, and
# complete the segment.  Save the file under a name restore_command ignores;
# it will be made available only after recovery refuses the TL1 copy.
$node_standby->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
my $target_lsn =
  $node_standby->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
my $child_walfile = $node_standby->safe_psql('postgres',
	"SELECT pg_walfile_name('$target_lsn'::pg_lsn)");
$node_standby->safe_psql('postgres', 'SELECT pg_switch_wal()');

my $archive = $node_primary->archive_dir;
my $staged_child = "$archive/$child_walfile.ready";
copy($node_standby->data_dir . "/pg_wal/$child_walfile", $staged_child)
  or die "Could not stage $child_walfile: $!";

# Continue TL1 past the switch point and archive its divergent version of the
# same segment.
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (-1)');
my $parent_walfile = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$parent_walfile' FROM pg_stat_archiver")
  or die "Timed out waiting for TL1 WAL to be archived";

# Publish only the history file.  Thus recovery knows about TL2, while only
# the divergent TL1 copy of the switch-point segment is initially available.
copy($node_standby->data_dir . '/pg_wal/00000002.history',
	"$archive/00000002.history")
  or die "Could not copy 00000002.history: $!";

ok(-f "$archive/$parent_walfile", 'parent switch-point segment is archived');
ok(!-f "$archive/$child_walfile", 'child switch-point segment is absent');
is(substr($parent_walfile, 8), substr($child_walfile, 8),
	'parent and child files have the same segment number');

$node_primary->stop;
$node_standby->stop;

my $node_rec = PostgreSQL::Test::Cluster->new('recovering');
$node_rec->init_from_backup($node_primary, 'backup', has_restoring => 1);
$node_rec->enable_restoring($node_primary, 1);
$node_rec->append_conf('postgresql.conf', <<EOM);
recovery_target_timeline = '2'
recovery_target_lsn = '$target_lsn'
recovery_target_action = 'promote'
wal_retrieve_retry_interval = '100ms'
log_min_messages = debug1
EOM
$node_rec->start;

# Prove that recovery reached the interesting segment and refused to use the
# older timeline, rather than merely observing that no bad restore occurred.
$node_rec->wait_for_log(
	qr/not searching older timelines for WAL segment "\Q$child_walfile\E"/);

# Make the correct file available and prove that recovery can proceed along
# TL2 to the requested target without replaying TL1's divergent row.
copy($staged_child, "$archive/$child_walfile")
  or die "Could not publish $child_walfile: $!";
$node_rec->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out waiting for recovery to reach TL2 target";
is($node_rec->safe_psql('postgres',
	q{SELECT string_agg(i::text, ',' ORDER BY i) FROM t}),
	'1,2', 'recovery followed TL2 without replaying divergent TL1 WAL');

done_testing();
