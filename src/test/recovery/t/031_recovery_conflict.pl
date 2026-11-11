# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test that connections to a hot standby are correctly canceled when a
# recovery conflict is detected Also, test that statistics in
# pg_stat_database_conflicts are populated correctly

use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


# Set up nodes
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

my $tablespace1 = "test_recovery_conflict_tblspc";

$node_primary->append_conf(
	'postgresql.conf', qq[
allow_in_place_tablespaces = on
log_temp_files = 0

# The test must control when pruning WAL is generated.
autovacuum = off

# for deadlock test
max_prepared_transactions = 10

# wait some to test the wait paths as well, but not long for obvious reasons
max_standby_streaming_delay = 50ms

temp_tablespaces = $tablespace1
# Some of the recovery conflict logging code only gets exercised after
# deadlock_timeout. The test doesn't rely on that additional output, but it's
# nice to get some minimal coverage of that code.
log_recovery_conflict_waits = on
deadlock_timeout = 10ms
]);
$node_primary->start;

my $backup_name = 'my_backup';

$node_primary->safe_psql('postgres',
	qq[CREATE TABLESPACE $tablespace1 LOCATION '']);

$node_primary->backup($backup_name);
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);

$node_standby->start;

my $test_db = "test_db";

# use a new database, to trigger database recovery conflict
$node_primary->safe_psql('postgres', "CREATE DATABASE $test_db");

# test schema / data
my $table1 = "test_recovery_conflict_table1";
my $table2 = "test_recovery_conflict_table2";
$node_primary->safe_psql(
	$test_db, qq[
CREATE TABLE ${table1}(a int, b int);
INSERT INTO $table1 SELECT i % 3, 0 FROM generate_series(1,20) i;
CREATE TABLE ${table2}(a int, b int);
]);
$node_primary->wait_for_replay_catchup($node_standby);


# a longrunning psql that we can use to trigger conflicts
my $psql_standby =
  $node_standby->background_psql($test_db, on_error_stop => 0);
my $expected_conflicts = 0;


## RECOVERY CONFLICT 1: Buffer pin conflict
my $sect = "buffer pin conflict";
$expected_conflicts++;

# Aborted INSERT on primary that will be cleaned up by vacuum. Has to be old
# enough so that there's not a snapshot conflict before the buffer pin
# conflict.

$node_primary->safe_psql(
	$test_db,
	qq[
	BEGIN;
	INSERT INTO $table1 VALUES (1,0);
	ROLLBACK;
	-- ensure flush, rollback doesn't do so
	BEGIN; LOCK $table1; COMMIT;
	]);

$node_primary->wait_for_replay_catchup($node_standby);

my $cursor1 = "test_recovery_conflict_cursor";

# DECLARE and use a cursor on standby, causing buffer with the only block of
# the relation to be pinned on the standby
my $res = $psql_standby->query_safe(
	qq[
    BEGIN;
    DECLARE $cursor1 CURSOR FOR SELECT b FROM $table1;
    FETCH FORWARD FROM $cursor1;
]);
# FETCH FORWARD should have returned a 0 since all values of b in the table
# are 0
like($res, qr/^0$/m, "$sect: cursor with conflicting pin established");

# to check the log starting now for recovery conflict messages
my $log_location = -s $node_standby->logfile;

# VACUUM FREEZE on the primary
$node_primary->safe_psql($test_db, qq[VACUUM FREEZE $table1;]);

# Wait for catchup. Existing connection will be terminated before replay is
# finished, so waiting for catchup ensures that there is no race between
# encountering the recovery conflict which causes the disconnect and checking
# the logfile for the terminated connection.
$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log("User was holding shared buffer pin for too long");
$psql_standby->reconnect_and_clear();
check_conflict_stat("bufferpin");


## RECOVERY CONFLICT 2: Snapshot conflict
$sect = "snapshot conflict";
$expected_conflicts++;

$node_primary->safe_psql($test_db,
	qq[INSERT INTO $table1 SELECT i, 0 FROM generate_series(1,20) i]);
$node_primary->wait_for_replay_catchup($node_standby);

# DECLARE and FETCH from cursor on the standby
$res = $psql_standby->query_safe(
	qq[
        BEGIN;
        DECLARE $cursor1 CURSOR FOR SELECT b FROM $table1;
        FETCH FORWARD FROM $cursor1;
        ]);
like($res, qr/^0$/m, "$sect: cursor with conflicting snapshot established");

# Do some HOT updates
$node_primary->safe_psql($test_db,
	qq[UPDATE $table1 SET a = a + 1 WHERE a > 2;]);

# VACUUM FREEZE, pruning those dead tuples
$node_primary->safe_psql($test_db, qq[VACUUM FREEZE $table1;]);

# Wait for attempted replay of PRUNE records
$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log(
	"User query might have needed to see row versions that must be removed");
$psql_standby->reconnect_and_clear();
check_conflict_stat("snapshot");


## RECOVERY CONFLICT 3: Lock conflict
$sect = "lock conflict";
$expected_conflicts++;

# acquire lock to conflict with
$res = $psql_standby->query_safe(
	qq[
        BEGIN;
        LOCK TABLE $table1 IN ACCESS SHARE MODE;
        SELECT 1;
        ]);
like($res, qr/^1$/m, "$sect: conflicting lock acquired");

# DROP TABLE containing block which standby has in a pinned buffer
$node_primary->safe_psql($test_db, qq[DROP TABLE $table1;]);

$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log("User was holding a relation lock for too long");
$psql_standby->reconnect_and_clear();
check_conflict_stat("lock");


## RECOVERY CONFLICT 4: Tablespace conflict
$sect = "tablespace conflict";
$expected_conflicts++;

# DECLARE a cursor for a query which, with sufficiently low work_mem, will
# spill tuples into temp files in the temporary tablespace created during
# setup.
$res = $psql_standby->query_safe(
	qq[
        BEGIN;
        SET work_mem = '64kB';
        DECLARE $cursor1 CURSOR FOR
          SELECT count(*) FROM generate_series(1,6000);
        FETCH FORWARD FROM $cursor1;
        ]);
like($res, qr/^6000$/m,
	"$sect: cursor with conflicting temp file established");

# Drop the tablespace currently containing spill files for the query on the
# standby
$node_primary->safe_psql($test_db, qq[DROP TABLESPACE $tablespace1;]);

$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log(
	"User was or might have been using tablespace that must be dropped");
$psql_standby->reconnect_and_clear();
check_conflict_stat("tablespace");


## RECOVERY CONFLICT 5: Deadlock
$sect = "startup deadlock";
$expected_conflicts++;

# Want to test recovery deadlock conflicts, not buffer pin conflicts. Without
# changing max_standby_streaming_delay it'd be timing dependent what we hit
# first
$node_standby->adjust_conf(
	'postgresql.conf',
	'max_standby_streaming_delay',
	"${PostgreSQL::Test::Utils::timeout_default}s");
$node_standby->restart();
$psql_standby->reconnect_and_clear();

# Generate a few dead rows, to later be cleaned up by vacuum. Then acquire a
# lock on another relation in a prepared xact, so it's held continuously by
# the startup process. The standby psql will block acquiring that lock while
# holding a pin that vacuum needs, triggering the deadlock.
$node_primary->safe_psql(
	$test_db,
	qq[
CREATE TABLE $table1(a int, b int);
INSERT INTO $table1 VALUES (1);
BEGIN;
INSERT INTO $table1(a) SELECT generate_series(1, 100) i;
ROLLBACK;
BEGIN;
LOCK TABLE $table2;
PREPARE TRANSACTION 'lock';
INSERT INTO $table1(a) VALUES (170);
SELECT txid_current();
]);

$node_primary->wait_for_replay_catchup($node_standby);

$res = $psql_standby->query_until(
	qr/^1$/m, qq[
    BEGIN;
    -- hold pin
    DECLARE $cursor1 CURSOR FOR SELECT a FROM $table1;
    FETCH FORWARD FROM $cursor1;
    -- wait for lock held by prepared transaction
	SELECT * FROM $table2;
    ]);
ok(1,
	"$sect: cursor holding conflicting pin, also waiting for lock, established"
);

# just to make sure we're waiting for lock already
ok( $node_standby->poll_query_until(
		'postgres', qq[
SELECT 'waiting' FROM pg_locks WHERE locktype = 'relation' AND NOT granted;
], 'waiting'),
	"$sect: lock acquisition is waiting");

# VACUUM FREEZE will prune away rows, causing a buffer pin conflict, while
# standby psql is waiting on lock
$node_primary->safe_psql($test_db, qq[VACUUM FREEZE $table1;]);
$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log("User transaction caused buffer deadlock with recovery.");
$psql_standby->reconnect_and_clear();
check_conflict_stat("deadlock");

# clean up for next tests
$node_primary->safe_psql($test_db, qq[ROLLBACK PREPARED 'lock';]);
$node_standby->adjust_conf('postgresql.conf', 'max_standby_streaming_delay',
	'50ms');
$node_standby->restart();
$psql_standby->reconnect_and_clear();


# Check that at least the expected number of conflicts show in
# pg_stat_database. Needs to be tested before database is dropped, for obvious
# reasons. A backend can process the same recovery conflict more than once
# before it exits.
cmp_ok( $node_standby->safe_psql(
		$test_db,
		qq[SELECT conflicts FROM pg_stat_database WHERE datname='$test_db';]),
	'>=',
	$expected_conflicts,
	qq[at least $expected_conflicts recovery conflicts shown in pg_stat_database]
);


## RECOVERY CONFLICT 6: Database conflict
$sect = "database conflict";

$node_primary->safe_psql('postgres', qq[DROP DATABASE $test_db;]);

$node_primary->wait_for_replay_catchup($node_standby);

check_conflict_log("User was connected to a database that must be dropped");


# explicitly shut down psql instances gracefully - to avoid hangs or worse on
# windows
$psql_standby->quit;

$node_standby->stop();
$node_primary->stop();


## RECOVERY CONFLICT 7: Progress reporting survives conflicts
#
# The startup process logs "redo in progress" every
# log_startup_progress_interval. It must keep doing so after replay has
# waited out a recovery conflict.
#
# The resolvers for lock and buffer pin conflicts,
# ResolveRecoveryConflictWithLock() and ResolveRecoveryConflictWithBufferPin(),
# set up timeouts of their own and cancel them when the conflict is resolved.
# They must not also cancel STARTUP_PROGRESS_TIMEOUT.  This section checks
# both resolvers' paths.
#
# The test doesn't depend on how fast replay runs. The restore_command,
# restore_on_gate_open, holds the startup process at two WAL segment boundaries
# until the test releases it:
#
#   hold 1 -> [segment with the conflicting record] -> hold 2 -> [next segment]
#
# A held startup process can't log, but its progress timer keeps running and
# sets a flag once the interval has elapsed.  The first record replayed after
# a hold then logs "redo in progress", if the timer is still armed.  So:
#
#  1. At hold 1, a session takes the lock or pin that replay of the
#     conflicting record needs.  After the hold is released, progress is
#     reported (the timer is armed), and replay then waits for the session.
#  2. The session ends its transaction.  Replay resolves the conflict, which
#     is where the timer used to be canceled, and reaches hold 2.
#  3. After hold 2 is released, progress is reported only if the timer
#     survived the conflict.
#
# The nodes here do archive recovery (recovery.signal) with hot_standby on:
# redo progress is never reported in standby mode, so the streaming standby
# tests above can't show this.
$sect = "progress after conflict";

{
	my $table_lock = 'test_recovery_conflict_progress_lock';
	my $table_pin = 'test_recovery_conflict_progress_pin';
	my $backup_progress = 'backup_progress';

	# Value of log_startup_progress_interval.
	my $progress_interval_ms = 10;

	# How long to keep the startup process held, so that the progress interval
	# has elapsed by the time it's released.  This is not a race: the startup
	# process can't move while it's held, and the progress timer is a kernel
	# timer that fires during the hold whatever the load, so this only has to
	# be longer than the interval.  The margin covers timer slack.
	my $hold_usec = 3 * $progress_interval_ms * 1000;

	# Our own primary, which archives its WAL and has none of the settings
	# above (tablespaces, short standby delays).  Without autovacuum, the only
	# pruning is the one the test asks for.
	my $node_wal = PostgreSQL::Test::Cluster->new('progress_primary');
	$node_wal->init(allows_streaming => 1, has_archiving => 1);
	$node_wal->append_conf('postgresql.conf', 'autovacuum = off');
	$node_wal->start;
	$node_wal->safe_psql('postgres', "CREATE DATABASE $test_db");

	# Table for the lock conflict.
	$node_wal->safe_psql($test_db, qq[CREATE TABLE $table_lock (a int)]);

	# Table for the buffer pin conflict: one page, with a dead row from an
	# aborted insert for VACUUM to prune.  Replaying the prune needs a cleanup
	# lock on that page, which a pin held by a session blocks.  As in section
	# 1, these events happen before anything that the session's snapshot can see,
	# so replay of the prune has no snapshot conflict with it, only the buffer
	# pin conflict.
	$node_wal->safe_psql(
		$test_db, qq[
CREATE TABLE $table_pin (a int, b int);
INSERT INTO $table_pin SELECT i, 0 FROM generate_series(1, 20) i;
BEGIN;
INSERT INTO $table_pin VALUES (21, 0);
ROLLBACK;
]);

	# The base backup is where recovery starts.  Stopping a backup switches
	# to a new WAL segment, so everything below is in segments that follow
	# the end of the backup, and recovery is consistent before reaching them.
	$node_wal->backup($backup_progress);

	# Run $sql, which writes the record that conflicts with replay, so that
	# the record lands in a WAL segment of its own and some other record lands
	# in the next segment.  The startup process will be held at the start of
	# each of the two segments.
	#
	# Returns a hash with:
	#  - conflict_lsn: the insert position before $sql ran.  Replay must not
	#    have reached it when the session takes its lock or pin.
	#  - seg, seg_after: file names of the two segments, for their gates.
	#  - seg_start, seg_after_start: where the two segments start.  Replay has
	#    reached hold 1 (or hold 2) when it has replayed WAL up to there.
	#
	# All of these are computed from one insert position taken after $sql
	# ran, which is inside the first segment.  pg_walfile_name() gives a
	# different answer for a position exactly at a segment boundary on older
	# branches, so it's only called on positions inside a segment.
	my $write_conflict_segment = sub {
		my $sql = shift;

		my $conflict_lsn = $node_wal->safe_psql($test_db,
			'SELECT pg_current_wal_insert_lsn()');
		$node_wal->safe_psql($test_db, $sql);
		my ($seg, $seg_after, $seg_start, $seg_after_start) = split(
			/\|/,
			$node_wal->safe_psql(
				$test_db, qq[
SELECT pg_walfile_name(lsn),
       pg_walfile_name(lsn + seg_size),
       seg_start,
       seg_start + seg_size
FROM pg_current_wal_insert_lsn() lsn,
     LATERAL (SELECT setting::numeric AS seg_size
              FROM pg_settings WHERE name = 'wal_segment_size') s,
     LATERAL (SELECT lsn - ((lsn - '0/0') % seg_size) AS seg_start) b]));
		# completes each segment, so that it gets archived
		$node_wal->safe_psql($test_db, 'SELECT pg_switch_wal()');
		$node_wal->advance_wal(1);

		return {
			conflict_lsn => $conflict_lsn,
			seg => $seg,
			seg_after => $seg_after,
			seg_start => $seg_start,
			seg_after_start => $seg_after_start,
		};
	};

	# Replaying a LOCK TABLE makes the startup process take
	# AccessExclusiveLock on the table.
	my $wal_lock =
	  $write_conflict_segment->(qq[BEGIN; LOCK TABLE $table_lock; COMMIT;]);

	# Replaying the prune record of this VACUUM makes the startup process
	# take a cleanup lock on the table's page.
	my $wal_pin = $write_conflict_segment->(qq[VACUUM FREEZE $table_pin;]);

	# A clean shutdown archives every completed segment before it finishes,
	# so after this the archive holds all the WAL we wrote.  The primary is
	# not needed again.
	$node_wal->stop;

	my $perlbin = $^X;
	my $archive = $node_wal->archive_dir;
	if ($windows_os)
	{
		$perlbin =~ s!\\!/!g;
		$archive =~ s!\\!/!g;
	}
	my $restore_timeout = $PostgreSQL::Test::Utils::timeout_default;

	# Wait until the startup process is held at the gate of the segment
	# starting at $seg_start: it has replayed everything before that segment,
	# and is waiting in restore_command for it.  The gate of that segment is
	# closed, so it stays there.  Returns the size of the server log then.
	my $wait_for_hold = sub {
		my ($node, $seg_start) = @_;

		$node->poll_query_until(
			$test_db, qq[
SELECT wait_event = 'RestoreCommand'
       AND pg_last_wal_replay_lsn() >= '$seg_start'
FROM pg_stat_activity
WHERE backend_type = 'startup']
		) or die "timed out waiting for the startup process to be held";

		return -s $node->logfile;
	};

	# Restore a node from the backup and the archive, hold what replay of the
	# conflicting record in $wal (from $write_conflict_segment) needs by
	# running $hold_sql in a session, and check progress reporting around the
	# resulting conflict.  $wait_event is what the startup process is
	# expected to wait on during the conflict, and $kind names the conflict.
	my $check_progress_after_conflict = sub {
		my ($name, $kind, $wal, $hold_sql, $wait_event) = @_;
		my $what = "$sect ($kind)";

		# Close the gates of both segments.
		my $gates = PostgreSQL::Test::Utils::tempdir('gates');
		my $gate_conflict = "$gates/$wal->{seg}.gate";
		my $gate_after = "$gates/$wal->{seg_after}.gate";
		append_to_file($gate_conflict, '');
		append_to_file($gate_after, '');
		$gates =~ s!\\!/!g if $windows_os;

		# recovery.signal, not standby.signal: this is archive recovery, not
		# standby mode, so redo progress is reported.
		my $node_pitr = PostgreSQL::Test::Cluster->new($name);
		$node_pitr->init_from_backup($node_wal, $backup_progress);
		$node_pitr->set_recovery_mode;

		# The backup copied the primary's postgresql.conf. Later lines overwrite.
		#  - archive_mode = off: don't archive into the primary's archive
		#    after recovery ends.
		#  - recovery_prefetch = off: don't read WAL ahead of replay, which
		#    could ask for a held segment too early.
		#  - max_standby_archive_delay = -1: wait for the conflicting session
		#    instead of canceling it, so the test decides when it ends.
		$node_pitr->append_conf(
			'postgresql.conf', qq[
archive_mode = off
hot_standby = on
recovery_prefetch = off
restore_command = '"$perlbin" "$FindBin::RealBin/restore_on_gate_open" "$archive" "$gates" $restore_timeout "%f" "%p"'
log_startup_progress_interval = ${progress_interval_ms}ms
max_standby_archive_delay = -1
]);

		# pg_ctl returns once the node accepts read-only connections, which
		# happens at consistency.  Replay then goes on until hold 1.
		$node_pitr->start;
		my $log_offset = $wait_for_hold->($node_pitr, $wal->{seg_start});

		# Hold the lock or pin that replay of the conflicting record needs.
		# The startup process can't have got there: it's held.
		my $psql_pitr = $node_pitr->background_psql($test_db);
		$psql_pitr->query_safe($hold_sql);
		is( $node_pitr->safe_psql(
				$test_db,
				qq[SELECT pg_last_wal_replay_lsn() < '$wal->{conflict_lsn}']),
			't',
			"$what: conflict set up before replay reached its record");

		# Let the progress interval elapse while the startup process is held,
		# and release it.  It logs progress for the first record it replays,
		# and then waits for our session.
		Time::HiRes::usleep($hold_usec);
		unlink($gate_conflict) or die "could not remove $gate_conflict: $!";
		$node_pitr->wait_for_event('startup', $wait_event);

		# Without progress before the conflict, the check after it would
		# prove nothing.
		like(
			slurp_file($node_pitr->logfile, $log_offset),
			qr/redo in progress/,
			"$what: progress reported before the conflict");

		# End our transaction, which releases the lock or pin.  Replay
		# resolves the conflict, finishes the segment, and reaches hold 2.
		#
		# Count progress from hold 2 on, not from the end of the conflict.
		# The timer may have expired during the conflict, before the resolver
		# canceled it, and the flag it set is still consumed by the next
		# record: one "redo in progress" line can follow the conflict even
		# without the fix.  At least the XLOG_SWITCH record comes after the
		# conflicting record in its segment, so that line is always logged
		# before hold 2.
		$psql_pitr->query_safe('COMMIT');
		$psql_pitr->quit;
		$log_offset = $wait_for_hold->($node_pitr, $wal->{seg_after_start});

		# Let the interval elapse again, and release the startup process for
		# the rest of the archive.  Recovery ends after the last segment in
		# the archive.
		Time::HiRes::usleep($hold_usec);
		unlink($gate_after) or die "could not remove $gate_after: $!";
		$node_pitr->poll_query_until($test_db,
			'SELECT NOT pg_is_in_recovery()')
		  or die "timed out waiting for recovery to end";

		# The actual check: fails if the conflict canceled the progress
		# timeout.
		my $n_after = () =
		  slurp_file($node_pitr->logfile, $log_offset) =~ /redo in progress/g;
		cmp_ok($n_after, '>', 0,
			"$what: progress reported after the conflict");

		$node_pitr->stop;
	};

	# Lock conflict: ResolveRecoveryConflictWithLock().  Hold ACCESS SHARE on
	# the table, which replay of the LOCK TABLE record has to wait for.  The
	# startup process waits for a relation lock.
	$check_progress_after_conflict->(
		'progress_lock', 'lock', $wal_lock,
		qq[BEGIN; LOCK TABLE $table_lock IN ACCESS SHARE MODE;], 'relation');

	# Buffer pin conflict: ResolveRecoveryConflictWithBufferPin().  A cursor
	# positioned on the table's only page keeps it pinned, and replay of
	# VACUUM's prune record has to wait for the pin to go away.  The startup
	# process waits for a cleanup lock on the buffer.
	$check_progress_after_conflict->(
		'progress_pin', 'buffer pin', $wal_pin,
		qq[BEGIN;
		   DECLARE progress_cursor CURSOR FOR SELECT b FROM $table_pin;
		   FETCH FORWARD FROM progress_cursor;],
		'BufferCleanup');
}


done_testing();

sub check_conflict_log
{
	my $message = shift;
	my $old_log_location = $log_location;

	$log_location = $node_standby->wait_for_log(qr/$message/, $log_location);

	cmp_ok($log_location, '>', $old_log_location,
		"$sect: logfile contains terminated connection due to recovery conflict"
	);
}

sub check_conflict_stat
{
	my $conflict_type = shift;

	ok( $node_standby->poll_query_until(
			$test_db,
			qq[SELECT confl_$conflict_type > 0 FROM pg_stat_database_conflicts WHERE datname='$test_db';],
			't'),
		"$sect: stats show conflict on standby");
}
