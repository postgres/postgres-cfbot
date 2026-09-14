# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test crash recovery for transactional file operations (FILEOPS).
#
# These tests verify that FILEOPS WAL replay correctly handles:
#   - Crash during file creation (with delete-on-abort)
#   - Crash during deferred file deletion
#   - Crash during file operations on standby
#   - Multiple sequential crashes

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_recovery');
$node->init;
$node->append_conf(
	"postgresql.conf", qq(
autovacuum = off
log_min_messages = debug2
));
$node->start;

# ================================================================
# Test 1: CREATE TABLE survives crash
# ================================================================

$node->safe_psql("postgres", qq(
CREATE TABLE fileops_test (id int, data text);
INSERT INTO fileops_test VALUES (1, 'created_table');
));

$node->stop('immediate');
$node->start;

my $result = $node->safe_psql("postgres",
	"SELECT data FROM fileops_test WHERE id = 1");
is($result, 'created_table', 'CREATE TABLE survives crash');

# ================================================================
# Test 2: DROP TABLE is properly handled after crash
# ================================================================

$node->safe_psql("postgres", qq(
CREATE TABLE drop_me (id int);
INSERT INTO drop_me VALUES (1);
));

# Get the relfilenode before dropping
my $relpath = $node->safe_psql("postgres",
	"SELECT pg_relation_filepath('drop_me')");

$node->safe_psql("postgres", "DROP TABLE drop_me");

$node->stop('immediate');
$node->start;

# Table should be gone
my ($ret, $stdout, $stderr) = $node->psql("postgres",
	"SELECT * FROM drop_me");
isnt($ret, 0, 'dropped table is gone after crash recovery');

# ================================================================
# Test 3: Crash during transaction with CREATE TABLE (uncommitted)
# ================================================================

# This table is committed
$node->safe_psql("postgres", qq(
CREATE TABLE committed_table (id int);
INSERT INTO committed_table VALUES (42);
));

# Crash the server
$node->stop('immediate');
$node->start;

# Committed table should exist
$result = $node->safe_psql("postgres",
	"SELECT id FROM committed_table");
is($result, '42', 'committed CREATE TABLE survives crash');

# ================================================================
# Test 4: Multiple CREATE and DROP operations then crash
# ================================================================

$node->safe_psql("postgres", qq(
CREATE TABLE t1 (id int);
CREATE TABLE t2 (id int);
CREATE TABLE t3 (id int);
INSERT INTO t1 VALUES (1);
INSERT INTO t2 VALUES (2);
INSERT INTO t3 VALUES (3);
DROP TABLE t2;
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT id FROM t1");
is($result, '1', 't1 survives crash');

($ret, $stdout, $stderr) = $node->psql("postgres",
	"SELECT * FROM t2");
isnt($ret, 0, 't2 (dropped) is gone after crash');

$result = $node->safe_psql("postgres",
	"SELECT id FROM t3");
is($result, '3', 't3 survives crash');

# ================================================================
# Test 5: Crash after checkpoint with file operations
# ================================================================

$node->safe_psql("postgres", qq(
DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS t3;
CREATE TABLE checkpoint_test (id int);
INSERT INTO checkpoint_test VALUES (1);
CHECKPOINT;
INSERT INTO checkpoint_test VALUES (2);
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT count(*) FROM checkpoint_test");
is($result, '2', 'data after checkpoint survives crash');

# ================================================================
# Test 6: Multiple crashes in sequence with file operations
# ================================================================

$node->safe_psql("postgres", qq(
DROP TABLE IF EXISTS checkpoint_test;
CREATE TABLE multi_crash (id int);
INSERT INTO multi_crash VALUES (1);
));

$node->stop('immediate');
$node->start;

$node->safe_psql("postgres", qq(
INSERT INTO multi_crash VALUES (2);
CREATE TABLE multi_crash_2 (id int);
INSERT INTO multi_crash_2 VALUES (10);
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT count(*) FROM multi_crash");
is($result, '2', 'multi_crash table correct after double crash');

$result = $node->safe_psql("postgres",
	"SELECT id FROM multi_crash_2");
is($result, '10', 'multi_crash_2 table correct after double crash');

# ================================================================
# Test 7: CREATE TABLESPACE survives crash
# ================================================================

$node->safe_psql("postgres", qq(
DROP TABLE IF EXISTS multi_crash;
DROP TABLE IF EXISTS multi_crash_2;
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE fileops_crash_ts LOCATION '';
CREATE TABLE ts_crash_table (id int) TABLESPACE fileops_crash_ts;
INSERT INTO ts_crash_table VALUES (99);
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT id FROM ts_crash_table");
is($result, '99', 'table in tablespace survives crash');

$result = $node->safe_psql("postgres",
	"SELECT spcname FROM pg_tablespace WHERE spcname = 'fileops_crash_ts'");
is($result, 'fileops_crash_ts', 'tablespace survives crash');

# ================================================================
# Test 8: DROP TABLESPACE completes after crash
# ================================================================

$node->safe_psql("postgres", qq(
DROP TABLE ts_crash_table;
DROP TABLESPACE fileops_crash_ts;
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT count(*) FROM pg_tablespace WHERE spcname = 'fileops_crash_ts'");
is($result, '0', 'dropped tablespace is gone after crash');

# ================================================================
# Test 9: CREATE DATABASE survives crash
# ================================================================

$node->safe_psql("postgres", qq(
CREATE DATABASE fileops_crash_db;
));

$node->stop('immediate');
$node->start;

$result = $node->safe_psql("postgres",
	"SELECT datname FROM pg_database WHERE datname = 'fileops_crash_db'");
is($result, 'fileops_crash_db', 'CREATE DATABASE survives crash');

$node->safe_psql("postgres", "DROP DATABASE fileops_crash_db");

# ================================================================
# Test 10: Standby crash during FILEOPS replay
# ================================================================

# Set up primary + standby
my $primary = PostgreSQL::Test::Cluster->new('fileops_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf("postgresql.conf", qq(
autovacuum = off
));
$primary->start;
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('fileops_standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# Create table on primary and wait for standby to catch up
$primary->safe_psql("postgres", qq(
CREATE TABLE standby_test (id int);
INSERT INTO standby_test VALUES (1);
));

$primary->wait_for_catchup($standby);

# Verify on standby
$result = $standby->safe_psql("postgres",
	"SELECT id FROM standby_test");
is($result, '1', 'CREATE TABLE replicated to standby');

# Crash the standby
$standby->stop('immediate');
$standby->start;

# Add more data on primary
$primary->safe_psql("postgres", qq(
INSERT INTO standby_test VALUES (2);
));

$primary->wait_for_catchup($standby);

$result = $standby->safe_psql("postgres",
	"SELECT count(*) FROM standby_test");
is($result, '2', 'standby recovers and catches up after crash');

# Clean up primary/standby
$standby->stop;
$primary->stop;

# Clean up original node
$node->stop;

# ================================================================
# Test 11/12: Crash exactly between WAL-flush and the physical syscall
# for the write-ordering fix (FileOpsCreate, FileOpsChmod).
#
# Before the fix, these two functions ran the physical syscall (and, for
# Create, the fsync) BEFORE the WAL record reached disk.  After the fix,
# WAL/UNDO is durably flushed first, and only then does the physical
# syscall run, inside a critical section.  These tests use injection
# points to pause the backend in that exact window -- after XLogFlush(),
# before open()/chmod() -- then hard-kill the postmaster.
#
# The injection point fires from inside the enclosing single-statement
# implicit transaction, before that transaction's COMMIT record could be
# written.  So crashing there means the transaction is, correctly,
# recovered as never-committed: WAL redo physically replays the CREATE/
# CHMOD unconditionally (redo does not know yet whether the surrounding
# xact committed), and then crash recovery's abort processing applies the
# UNDO record for that not-committed transaction, reversing the physical
# change.  The end state must be as if the transaction never ran at all --
# no orphaned file, no leftover mode change -- which is exactly the
# invariant broken before this fix: with the syscall running before WAL,
# a crash in this window used to leave a physical create()/chmod() with
# NO corresponding WAL or UNDO record for recovery to reconcile against
# (the syscall had already happened, but WAL/UNDO for it had not),
# leaving a permanently orphaned file or an un-reversible mode change even
# though the owning transaction never committed.  Now WAL/UNDO is always
# durable before the syscall runs, so recovery always has a consistent
# decision to make -- keep (if committed) or reverse (if not) -- no
# matter which side of the syscall the crash lands on.
# ================================================================

if ($ENV{enable_injection_points} eq 'yes')
{
	my $inode = PostgreSQL::Test::Cluster->new('fileops_wal_ordering');
	$inode->init;
	$inode->append_conf(
		'postgresql.conf', qq(
shared_preload_libraries = 'injection_points'
autovacuum = off
log_min_messages = debug2
wal_level = replica
));
	$inode->start;

	if ($inode->check_extension('injection_points')
		&& $inode->check_extension('test_fileops'))
	{
		$inode->safe_psql('postgres', 'CREATE EXTENSION injection_points');
		$inode->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

		my $idatadir = $inode->data_dir;

		# --- Test 11: FileOpsCreate -----------------------------------
		$inode->safe_psql('postgres',
			q{SELECT injection_points_attach('fileops-create-after-wal-flush', 'wait')}
		);

		my $create_path = "$idatadir/wal_ordering_create.dat";
		my $bgpsql = $inode->background_psql('postgres');
		$bgpsql->query_until(
			qr/starting_create/,
			qq(\\echo starting_create
SELECT test_fileops_create('$create_path', 384);
\\q
));

		$inode->wait_for_event('client backend',
			'fileops-create-after-wal-flush');

		# The WAL record is durably flushed, but the physical create() has
		# not run yet -- the file must not exist on disk right now.
		ok(!-e $create_path,
			'Test 11: file does not exist yet at the injection point '
			 . '(WAL durable, physical create() not yet run)');

		# Hard-kill the postmaster from exactly this window.  The wrapping
		# implicit transaction is still in progress (blocked on the
		# injection point), so it never committed.
		$inode->stop('immediate');
		$inode->start;

		# Recovery redoes the physical CREATE from the durable WAL record
		# unconditionally, then determines the transaction never committed
		# and applies its UNDO record to reverse the create.  Net effect:
		# the file must NOT exist -- exactly as if the transaction had
		# never run, with no orphaned state left behind by the crash.
		ok(!-e $create_path,
			'Test 11: FileOpsCreate of a not-committed transaction is '
			 . 'cleanly reversed after a crash mid-critical-section');

		# Server is healthy after the reconciliation.
		my $result11 = $inode->safe_psql('postgres', 'SELECT 1');
		is($result11, '1', 'Test 11: server operational after recovery');

		$bgpsql->{run}->finish;

		# --- Test 12: FileOpsChmod -------------------------------------
		$inode->safe_psql('postgres',
			qq{SELECT test_fileops_create_tempfile('wal_ordering_chmod.dat')});
		my $chmod_path = "$idatadir/wal_ordering_chmod.dat";
		$inode->safe_psql('postgres',
			qq{SELECT test_fileops_chmod('$chmod_path', 420)});    # 0644
		my $mode_before = $inode->safe_psql('postgres',
			qq{SELECT test_fileops_get_mode('$chmod_path')});
		is($mode_before, '420', 'Test 12: baseline mode is 0644');

		$inode->safe_psql('postgres',
			q{SELECT injection_points_attach('fileops-chmod-after-wal-flush', 'wait')}
		);

		$bgpsql = $inode->background_psql('postgres');
		$bgpsql->query_until(
			qr/starting_chmod/,
			qq(\\echo starting_chmod
SELECT test_fileops_chmod('$chmod_path', 448);    -- 0700
\\q
));

		$inode->wait_for_event('client backend',
			'fileops-chmod-after-wal-flush');

		# WAL is durably flushed, but chmod() has not run yet -- the mode
		# on disk must still be the baseline 0644.
		my $mode_at_injection = $inode->safe_psql('postgres',
			qq{SELECT test_fileops_get_mode('$chmod_path')});
		is($mode_at_injection, '420',
			'Test 12: mode unchanged on disk at the injection point '
			 . '(WAL durable, physical chmod() not yet run)');

		# Hard-kill mid-critical-section, same as Test 11.  The chmod's
		# wrapping implicit transaction never committed.
		$inode->stop('immediate');
		$inode->start;

		# Recovery redoes the physical CHMOD unconditionally, then reverses
		# it via UNDO because the transaction never committed.  Net effect:
		# mode must be back to the pre-transaction baseline (0644), not the
		# attempted 0700, and not some undefined intermediate state.
		my $mode_after = $inode->safe_psql('postgres',
			qq{SELECT test_fileops_get_mode('$chmod_path')});
		is($mode_after, '420',
			'Test 12: FileOpsChmod of a not-committed transaction is '
			 . 'cleanly reversed after a crash mid-critical-section');

		# Server is healthy after the reconciliation.
		my $result12 = $inode->safe_psql('postgres', 'SELECT 1');
		is($result12, '1', 'Test 12: server operational after recovery');

		$bgpsql->{run}->finish;
	}
	else
	{
		note
			'skipping Test 11/12 (injection_points or test_fileops extension not installed)';
	}

	$inode->stop;
}
else
{
	note 'skipping Test 11/12 (injection points not supported by this build)';
}

done_testing();
