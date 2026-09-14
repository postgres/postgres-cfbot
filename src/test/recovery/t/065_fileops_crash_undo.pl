# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# FILEOPS crash-recovery revert via the cluster-wide UNDO engine (common WAL).
#
# FILEOPS is a consumer of the cluster-wide UNDO engine: its structural
# operations (file existence, permissions, directory structure) are
# WAL-durable via RM_FILEOPS_ID forward records (fileops_redo), and each
# reversible op also writes an UNDO record into the common WAL
# (PrepareXactUndoData(UNDO_RMID_FILEOPS,...) / InsertXactUndoData) in the
# same critical section as its forward record.  Filesystem structural
# operations are cluster-wide changes whose durability/revert belong in the
# common WAL.  This test proves that model is crash-safe.
#
# A transaction produces FILEOPS create/chmod (immediate, WAL-flushed), then the
# server is crashed (immediate stop) BEFORE the transaction commits.  On
# recovery:
#   - fileops_redo() REDOES the forward create/chmod unconditionally, and
#   - PerformUndoRecovery() (end of redo) applies the never-committed
#     transaction's FILEOPS UNDO chain via the RM_FILEOPS_ID rm_undo
#     callbacks (fileops_undo.c), reversing every op:
#       - the created file is unlinked (CREATE revert), and
#       - the chmod'd file's mode is restored (CHMOD revert, before-state in WAL).
#
# FILEOPS only writes WAL at wal_level >= replica (XLogIsNeeded()), so the test
# runs at wal_level = replica, matching any FILEOPS deployment.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_level = replica
));
$node->start;

if (!$node->check_extension('test_fileops'))
{
	plan skip_all => 'Extension test_fileops not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $datadir = $node->data_dir;
my $create_path = "$datadir/fileops_crash_create.dat";
my $chmod_path = "$datadir/fileops_crash_chmod.dat";

# ---------------------------------------------------------------------------
# Set up a pre-existing file to chmod (this part is committed).  0644 = 420.
# ---------------------------------------------------------------------------
$node->safe_psql('postgres',
	q{SELECT test_fileops_create_tempfile('fileops_crash_chmod.dat')});
$node->safe_psql('postgres',
	qq{SELECT test_fileops_chmod('$chmod_path', 420)});
SKIP:
{
	skip 'POSIX permission semantics required', 1 if $windows_os;
	is( $node->safe_psql(
			'postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
		'420',
		'baseline mode of pre-existing file is 0644');
}

# ---------------------------------------------------------------------------
# Open a transaction that produces FILEOPS UNDO (create a new file and
# chmod the existing one) and DO NOT commit it.  The FILEOPS forward operations
# execute and are WAL-flushed immediately (the file exists on disk, the mode is
# changed), but the wrapping transaction stays in progress.
# ---------------------------------------------------------------------------
my $bg = $node->background_psql('postgres');
$bg->query_until(
	qr/fileops_ready/, qq(
BEGIN;
SELECT test_fileops_create('$create_path', 384);   -- 0600, new file
SELECT test_fileops_chmod('$chmod_path', 448);      -- 0700, existing file
\\echo fileops_ready
));

# The forward ops are visible on disk while the xact is in progress.
ok(-e $create_path,
	'created file exists on disk before crash (in-progress xact)');
SKIP:
{
	skip 'POSIX permission semantics required', 1 if $windows_os;
	is( $node->safe_psql(
			'postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
		'448',
		'chmod applied on disk before crash (in-progress xact)');
}

# ---------------------------------------------------------------------------
# Crash immediately: the transaction never commits.  Stop the server before
# closing the client: closing it first would let the backend roll the
# transaction back online, which is a different case.  The background psql
# connection dies with the server; abandon it (do not try to drain/finish it,
# which would hang waiting on the dead connection).
# ---------------------------------------------------------------------------
$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;

# ---------------------------------------------------------------------------
# Recovery redoes the forward ops from WAL, then PerformUndoRecovery() reverses
# the never-committed transaction's ops from its common-WAL UNDO chain:
#   - the created file is unlinked, and
#   - the chmod is restored to the original 0644.
# ---------------------------------------------------------------------------
ok( !-e $create_path,
	'FILEOPS COMMON-WAL CRASH RECOVERY: created file was REVERTED (unlinked) after crash'
);
SKIP:
{
	skip 'POSIX permission semantics required', 1 if $windows_os;
	is( $node->safe_psql(
			'postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
		'420',
		'FILEOPS COMMON-WAL CRASH RECOVERY: chmod was REVERTED (0700 -> 0644) after crash'
	);
}

# Recovery must be clean.
my $rlog = slurp_file($node->logfile);
unlike(
	$rlog,
	qr/PANIC|inconsistent|corrupt/i,
	'clean crash recovery, no PANIC/corruption');

# Server healthy afterwards.
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'server operational after crash-recovery undo');

$node->stop;
done_testing();
