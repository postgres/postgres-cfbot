# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A streaming standby promoted at a recovery target inside a WAL summary,
# after receiving WAL past the target, including a truncation.  A standby only
# summarizes replayed WAL, so the old-timeline summaries end at the switch
# point, and incremental backups after promotion restore the promoted
# server's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', <<EOF);
summarize_wal = on
autovacuum = off
EOF
$primary->start;

# One row per page, so that the heap fits in one segment even with the
# six-block RELSEG_SIZE used by CI.
my $block_size = $primary->safe_psql('postgres', 'SHOW block_size');
my $pad_size = $block_size / 2;
$primary->safe_psql('postgres', <<EOF);
CREATE TABLE t (id int, pad text STORAGE PLAIN);
INSERT INTO t SELECT g, repeat('x', $pad_size) FROM generate_series(1, 4) g;
VACUUM t;
EOF
$primary->backup('full');
my $full_path = $primary->backup_dir . '/full';

# Each recovery target is inside a WAL summary: after the redo pointer of a
# checkpoint and some WAL that the standby replays.
my $insert_lsn = 'SELECT pg_current_wal_insert_lsn()';
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres', "UPDATE t SET pad = pad || 'o' WHERE id > 2");
my $target1 = $primary->safe_psql('postgres', $insert_lsn);

# Past the first target, only new rows: harmless for that standby.
$primary->safe_psql('postgres',
	"INSERT INTO t VALUES (5, repeat('x', $pad_size))");
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres', 'CREATE TABLE marker ()');
my $target2 = $primary->safe_psql('postgres', $insert_lsn);

# Past the second target, a truncation from 7 blocks to 2.
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id > 2');
$primary->safe_psql('postgres', 'VACUUM t');
is($primary->safe_psql('postgres', "SELECT pg_relation_size('t') / $block_size"),
	'2', 'VACUUM truncates the heap after the second target');
$primary->safe_psql('postgres', 'CHECKPOINT');
my $end_lsn = $primary->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');

# Start a standby that pauses at the target and has received everything the
# primary wrote, then promote it.
sub promoted_standby
{
	my ($name, $target) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init_from_backup($primary, 'full', has_streaming => 1);
	$node->append_conf('postgresql.conf', <<EOF);
recovery_target_lsn = '$target'
recovery_target_inclusive = false
recovery_target_action = 'pause'
EOF
	$node->start;
	$node->poll_query_until('postgres',
		"SELECT pg_get_wal_replay_pause_state() = 'paused'")
	  or die "timed out waiting for $name to pause";
	$node->poll_query_until('postgres',
		"SELECT pg_last_wal_receive_lsn() >= '$end_lsn'")
	  or die "timed out waiting for $name to receive up to $end_lsn";
	$node->poll_query_until('postgres',
		"SELECT pending_lsn >= pg_last_wal_replay_lsn() FROM pg_get_wal_summarizer_state()")
	  or die "timed out waiting for $name to summarize up to replay";
	is( $node->safe_psql(
			'postgres',
			"SELECT pending_lsn <= pg_last_wal_replay_lsn() FROM pg_get_wal_summarizer_state()"),
		't',
		"$name: the summarizer does not read past replay");
	$node->promote;
	return $node;
}

my $standby1 = promoted_standby('standby1', $target1);
my $standby2 = promoted_standby('standby2', $target2);
$primary->stop;

# The tables are not read before the backups: with data checksums, setting
# hint bits would WAL-log their blocks again on the new timeline.
my $contents_sql = "SELECT id, right(pad, 1) FROM t ORDER BY id";

# First standby: only new rows follow its target.
$standby1->command_ok(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $primary->backup_dir . '/incr1',
		'--checkpoint' => 'fast',
		'--incremental' => "$full_path/backup_manifest",
	],
	'incremental backup when only new rows follow the target');
my $expected1 = "1|x\n2|x\n3|o\n4|o";
is($standby1->safe_psql('postgres', $contents_sql),
	$expected1, 'first standby has the rows replayed before the target');
$standby1->stop;
my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup(
	$primary, 'incr1',
	combine_with_prior => ['full'],
	combine_mode => $mode);
$restored->start;
is($restored->safe_psql('postgres', $contents_sql),
	$expected1, 'restored rows match the first standby');
$restored->stop;

# Second standby: the truncation after its target was received but never
# replayed or summarized, so the incremental backup restores the rows.
$standby2->command_ok(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $primary->backup_dir . '/incr2',
		'--checkpoint' => 'fast',
		'--incremental' => "$full_path/backup_manifest",
	],
	'incremental backup when a truncation follows the target');
my $expected2 = "1|x\n2|x\n3|o\n4|o\n5|x";
is($standby2->safe_psql('postgres', $contents_sql),
	$expected2, 'second standby has the rows replayed before the target');
$standby2->stop;
my $restored2 = PostgreSQL::Test::Cluster->new('restored2');
$restored2->init_from_backup(
	$primary, 'incr2',
	combine_with_prior => ['full'],
	combine_mode => $mode);
$restored2->start;
is($restored2->safe_psql('postgres', $contents_sql),
	$expected2, 'restored rows match the second standby');
$restored2->stop;

done_testing();
