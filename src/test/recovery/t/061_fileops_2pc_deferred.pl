# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test that DEFERRED commit-time FILEOPS (delete/rename) and abort-time
# filesystem undo survive PREPARE TRANSACTION across a crash.
#
# The bug: PostPrepare_FileOps() used to free the pending-op list without
# recording it anywhere, so a transaction PREPARE'd with a deferred delete or
# rename (at_commit=true) lost that op entirely -- COMMIT PREPARED performed
# nothing -- and an immediate create's abort-time undo was lost for ROLLBACK
# PREPARED.  The fix serializes the pending ops into the 2PC state file at
# PREPARE (TWOPHASE_RM_FILEOPS_ID records) and performs them in the FILEOPS
# post-commit / post-abort callbacks, so they survive a crash between PREPARE
# and the final decision, driven from the durable 2PC state.
#
# Unlike 063_fileops_2pc.pl (which only exercises chmod, a constructive op
# reversed via the cluster-wide UNDO chain), this test drives the deferred
# commit-time delete/rename path -- the one that was silently dropped.
#
# NB: FILEOPS writes WAL/UNDO only when XLogIsNeeded() (wal_level >= replica),
# so 2PC durability requires wal_level = replica; this test runs there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_2pc_deferred');
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

sub file_exists
{
	my ($path) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT test_fileops_file_exists('$path')});
}

# Poll until the file's existence matches $want ('t'/'f'); some reversals run
# through the async cluster-wide UNDO revert worker, not synchronously.
sub wait_for_exists
{
	my ($path, $want, $desc) = @_;
	my $ok = $node->poll_query_until('postgres',
		qq{SELECT test_fileops_file_exists('$path') = '$want'});
	is($ok, '1', $desc);
}

# ================================================================
# Scenario A: COMMIT PREPARED performs the deferred commit-time ops
#   - deferred delete of an existing file
#   - deferred rename of an existing file
#   - immediate create of a new file (register_delete)
# PREPARE, crash, restart, then COMMIT PREPARED.  All three must take effect.
# ================================================================
my $del_a = "$datadir/fo2pc_del_a.dat";
my $ren_a = "$datadir/fo2pc_ren_a.dat";
my $ren_a2 = "$datadir/fo2pc_ren_a.renamed";
my $new_a = "$datadir/fo2pc_new_a.dat";

$node->safe_psql('postgres',
	qq{SELECT test_fileops_create_tempfile('fo2pc_del_a.dat')});
$node->safe_psql('postgres',
	qq{SELECT test_fileops_create_tempfile('fo2pc_ren_a.dat')});

is(file_exists($del_a), 't', 'A: delete target exists before txn');
is(file_exists($ren_a), 't', 'A: rename source exists before txn');

$node->safe_psql(
	'postgres', qq(
BEGIN;
SELECT test_fileops_delete('$del_a');
SELECT test_fileops_rename('$ren_a', '$ren_a2');
SELECT test_fileops_create('$new_a', 384);
PREPARE TRANSACTION 'fo2pc_commit';
));

# While prepared, deferred ops have NOT run yet; the immediate create has.
is(file_exists($del_a), 't',
	'A: delete deferred -- file still present while prepared');
is(file_exists($ren_a), 't',
	'A: rename deferred -- source still present while prepared');
is(file_exists($new_a), 't', 'A: immediate create present while prepared');

# Crash between PREPARE and COMMIT PREPARED.
$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT gid FROM pg_prepared_xacts}),
	'fo2pc_commit', 'A: prepared xact survives crash');

$node->safe_psql('postgres', q{COMMIT PREPARED 'fo2pc_commit'});

is(file_exists($del_a), 'f',
	'A: COMMIT PREPARED performed the deferred delete');
is(file_exists($ren_a), 'f',
	'A: COMMIT PREPARED performed the rename (source gone)');
is(file_exists($ren_a2), 't',
	'A: COMMIT PREPARED performed the rename (dest present)');
is(file_exists($new_a), 't',
	'A: created file persists after COMMIT PREPARED');

# ================================================================
# Scenario B: ROLLBACK PREPARED does NOT perform the deferred commit-time
# ops, and DOES undo the immediate create.
# PREPARE, crash, restart, then ROLLBACK PREPARED.
# ================================================================
my $del_b = "$datadir/fo2pc_del_b.dat";
my $ren_b = "$datadir/fo2pc_ren_b.dat";
my $ren_b2 = "$datadir/fo2pc_ren_b.renamed";
my $new_b = "$datadir/fo2pc_new_b.dat";

$node->safe_psql('postgres',
	qq{SELECT test_fileops_create_tempfile('fo2pc_del_b.dat')});
$node->safe_psql('postgres',
	qq{SELECT test_fileops_create_tempfile('fo2pc_ren_b.dat')});

$node->safe_psql(
	'postgres', qq(
BEGIN;
SELECT test_fileops_delete('$del_b');
SELECT test_fileops_rename('$ren_b', '$ren_b2');
SELECT test_fileops_create('$new_b', 384);
PREPARE TRANSACTION 'fo2pc_rollback';
));

is(file_exists($new_b), 't', 'B: immediate create present while prepared');

# Crash between PREPARE and ROLLBACK PREPARED.
$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT gid FROM pg_prepared_xacts}),
	'fo2pc_rollback', 'B: prepared xact survives crash');

$node->safe_psql('postgres', q{ROLLBACK PREPARED 'fo2pc_rollback'});

is(file_exists($del_b), 't',
	'B: ROLLBACK PREPARED did NOT delete (file still present)');
is(file_exists($ren_b), 't',
	'B: ROLLBACK PREPARED did NOT rename (source still present)');
is(file_exists($ren_b2), 'f',
	'B: ROLLBACK PREPARED did NOT rename (dest absent)');
# The immediate create is undone by the constructive op's cluster-wide UNDO
# record via the async revert worker on ROLLBACK PREPARED, so poll.
wait_for_exists($new_b, 'f',
	'B: ROLLBACK PREPARED undid the immediate create');

# Server healthy at the end.
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'server operational after all deferred-2PC scenarios');

$node->stop;
done_testing();
