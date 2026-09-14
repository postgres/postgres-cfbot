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
# common WAL, replayed like heap's own records -- NOT in any table-AM
# per-backend UNDO log.  This test proves that model is crash-safe.
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
# No per-backend UNDO engine is involved.
#
# FILEOPS only writes WAL at wal_level >= replica (XLogIsNeeded()), so the test
# runs at wal_level = replica, matching any FILEOPS deployment.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_crash');
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
my $create_path = "$datadir/pbu_crash_create.dat";
my $chmod_path  = "$datadir/pbu_crash_chmod.dat";

# ---------------------------------------------------------------------------
# Set up a pre-existing file to chmod (this part is committed).  0644 = 420.
# ---------------------------------------------------------------------------
$node->safe_psql('postgres',
	q{SELECT test_fileops_create_tempfile('pbu_crash_chmod.dat')});
$node->safe_psql('postgres', qq{SELECT test_fileops_chmod('$chmod_path', 420)});
is($node->safe_psql('postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
	'420', 'baseline mode of pre-existing file is 0644');

# ---------------------------------------------------------------------------
# Open a transaction that produces per-backend UNDO (create a new file and
# chmod the existing one) and DO NOT commit it.  The FILEOPS forward operations
# execute and are WAL-flushed immediately (the file exists on disk, the mode is
# changed), but the wrapping transaction stays in progress.
# ---------------------------------------------------------------------------
my $bg = $node->background_psql('postgres');
$bg->query_until(
	qr/pbu_ready/, qq(
BEGIN;
SELECT test_fileops_create('$create_path', 384);   -- 0600, new file
SELECT test_fileops_chmod('$chmod_path', 448);      -- 0700, existing file
\\echo pbu_ready
));

# The forward ops are visible on disk while the xact is in progress.
ok(-e $create_path, 'created file exists on disk before crash (in-progress xact)');
is($node->safe_psql('postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
	'448', 'chmod applied on disk before crash (in-progress xact)');

# ---------------------------------------------------------------------------
# Crash immediately: the transaction never commits.  The background psql
# connection dies with the server; abandon it (do not try to drain/finish it,
# which would hang waiting on the dead connection).
# ---------------------------------------------------------------------------
$bg->{run}->kill_kill;
$node->stop('immediate');
$node->start;

# ---------------------------------------------------------------------------
# Recovery redoes the forward ops from WAL, then PerformUndoRecovery() reverses
# the never-committed transaction's ops from its common-WAL UNDO chain:
#   - the created file is unlinked, and
#   - the chmod is restored to the original 0644.
# ---------------------------------------------------------------------------
ok(!-e $create_path,
	'FILEOPS COMMON-WAL CRASH RECOVERY: created file was REVERTED (unlinked) after crash');
is($node->safe_psql('postgres', qq{SELECT test_fileops_get_mode('$chmod_path')}),
	'420',
	'FILEOPS COMMON-WAL CRASH RECOVERY: chmod was REVERTED (0700 -> 0644) after crash');

# Recovery must be clean and must NOT have engaged the per-backend UNDO engine
# for FILEOPS: the revert rides the cluster-wide UNDO engine on the common WAL.
my $rlog = slurp_file($node->logfile);
unlike($rlog, qr/per-backend UNDO recovery: rolling back transaction/,
	'FILEOPS revert did NOT go through the per-backend UNDO engine');
unlike($rlog, qr/PANIC|inconsistent|corrupt/i,
	'clean crash recovery, no PANIC/corruption');

# Server healthy afterwards.
is($node->safe_psql('postgres', 'SELECT 1'), '1',
	'server operational after per-backend crash-recovery undo');

$node->stop;
done_testing();
