# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test two-phase commit (PREPARE TRANSACTION / COMMIT PREPARED / ROLLBACK
# PREPARED) for transactions that generated cluster-wide FILEOPS UNDO.
#
# FILEOPS uses the undo-in-common-WAL mechanism.  At PREPARE the permanent
# UNDO chain-head LSN is durably saved in the 2PC state (xl_xact_prepare /
# gxact->undo_batch_lsn), and its WAL is retained while the xact stays
# prepared.  ROLLBACK PREPARED (even from a different backend, post-crash)
# hands that LSN to the async logical-revert machinery via ATMAddAborted(),
# which reverses the filesystem change.  COMMIT PREPARED keeps it.
#
# Vehicle: the test_fileops extension's SQL wrappers for FileOpsChmod, which
# are legal inside a transaction block that can then be PREPARE'd.
#
# NB: FILEOPS only writes UNDO WAL when XLogIsNeeded() (wal_level >= replica).
# At wal_level = minimal it relies on the backend-private pending-op list,
# which is discarded at PREPARE and cannot survive 2PC.  This test therefore
# runs at wal_level = replica, which is what any FILEOPS + 2PC deployment
# requires.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_2pc');
$node->init;
$node->append_conf(
	"postgresql.conf", qq(
autovacuum = off
max_prepared_transactions = 10
wal_level = replica
));
$node->start;

if (!$node->check_extension('test_fileops'))
{
	plan skip_all => 'Extension test_fileops not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $datadir = $node->data_dir;
my $file = "$datadir/twopc.dat";

# Poll until the file mode equals $want (async revert takes a moment) or fail.
sub wait_for_mode
{
	my ($want, $desc) = @_;
	my $ok = $node->poll_query_until('postgres',
		qq{SELECT test_fileops_get_mode('$file') = $want});
	is($ok, '1', $desc);
}

# 0644 = 420, 0700 = 448, 0600 = 384 (decimal)
$node->safe_psql('postgres',
	qq{SELECT test_fileops_create_tempfile('twopc.dat')});
$node->safe_psql('postgres', qq{SELECT test_fileops_chmod('$file', 420)});
my $baseline = $node->safe_psql('postgres',
	qq{SELECT test_fileops_get_mode('$file')});
is($baseline, '420', 'baseline mode is 0644');

# ================================================================
# Test 1: ROLLBACK PREPARED reverses the chmod
# ================================================================
$node->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_chmod('$file', 448);  -- 0700
PREPARE TRANSACTION 'fo_rb';
));
my $m = $node->safe_psql('postgres',
	qq{SELECT test_fileops_get_mode('$file')});
is($m, '448', 'Test 1: chmod applied and still visible while prepared');

$node->safe_psql('postgres', q{ROLLBACK PREPARED 'fo_rb'});
wait_for_mode(420, 'Test 1: ROLLBACK PREPARED reverses chmod (0700 -> 0644)');

# ================================================================
# Test 2: COMMIT PREPARED keeps the chmod
# ================================================================
$node->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_chmod('$file', 448);  -- 0700
PREPARE TRANSACTION 'fo_cp';
));
$node->safe_psql('postgres', q{COMMIT PREPARED 'fo_cp'});
$m = $node->safe_psql('postgres',
	qq{SELECT test_fileops_get_mode('$file')});
is($m, '448', 'Test 2: COMMIT PREPARED keeps chmod (0700)');

# Reset to baseline for the crash test.
$node->safe_psql('postgres', qq{SELECT test_fileops_chmod('$file', 420)});

# ================================================================
# Test 3: WAL retention across checkpoints while prepared
# ================================================================
$node->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_chmod('$file', 384);  -- 0600
PREPARE TRANSACTION 'fo_ckpt';
));
# Churn WAL and checkpoint; the prepared xact's UNDO batch must be retained.
$node->safe_psql('postgres', 'CHECKPOINT');
for my $i (1 .. 20)
{
	$node->safe_psql('postgres',
		"CREATE TABLE junk_$i(x int); "
		  . "INSERT INTO junk_$i SELECT g FROM generate_series(1,5000) g; "
		  . "DROP TABLE junk_$i;");
}
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', q{ROLLBACK PREPARED 'fo_ckpt'});
wait_for_mode(420,
	'Test 3: ROLLBACK PREPARED still reverses after WAL churn + checkpoints');

# ================================================================
# Test 4: crash recovery -- prepared UNDO xact survives, then reverts
# ================================================================
$node->safe_psql('postgres', qq{SELECT test_fileops_chmod('$file', 420)});
$node->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_chmod('$file', 448);  -- 0700
PREPARE TRANSACTION 'fo_crash';
));
$node->stop('immediate');
$node->start;

my $prep = $node->safe_psql('postgres',
	q{SELECT gid FROM pg_prepared_xacts});
is($prep, 'fo_crash', 'Test 4: prepared xact survives crash recovery');
$m = $node->safe_psql('postgres',
	qq{SELECT test_fileops_get_mode('$file')});
is($m, '448', 'Test 4: chmod still applied (prepared, not yet rolled back)');

$node->safe_psql('postgres', q{ROLLBACK PREPARED 'fo_crash'});
wait_for_mode(420,
	'Test 4: ROLLBACK PREPARED after crash+restart reverses chmod');

# Server healthy at the end.
is($node->safe_psql('postgres', 'SELECT 1'), '1',
	'server operational after all 2PC UNDO scenarios');

$node->stop;
done_testing();
