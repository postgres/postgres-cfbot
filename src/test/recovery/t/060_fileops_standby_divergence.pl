# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Regression test: physical-standby reproduction of FILEOPS deferred
# DESTRUCTIVE operations (DELETE, RENAME, RMDIR).
#
# These three ops execute on the primary at commit inside
# FileOpsDoPendingOps().  Each is logged at commit-EXECUTION time with a
# real, idempotent redo handler, so a physical standby (which only replays
# WAL, never running FileOpsDoPendingOps) reproduces them and its filesystem
# converges with the primary's.
#
# This test sets up a streaming physical standby, performs a committed
# deferred DELETE, RENAME, and RMDIR on the primary, waits for catchup, and
# asserts the standby's filesystem MATCHES the primary's.  It MUST FAIL on the
# pre-fix (no-op redo) code -- see the header note in fileops.c and the
# "prove non-vacuity" comment below.
#
# All FILEOPS paths are RELATIVE to the data directory, matching every core
# FILEOPS caller (dbcommands.c, tablespace.c): the backend's cwd is PGDATA, so
# a relative path resolves to the right file on both the primary and a standby
# whose absolute data_dir differs.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('fileops_div_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf("postgresql.conf", qq(
autovacuum = off
wal_level = replica
));
$primary->start;

if (!$primary->check_extension('test_fileops'))
{
	plan skip_all => 'Extension test_fileops not installed';
}
$primary->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $primary_dir = $primary->data_dir;

# Fixture, all committed and WAL-logged so the standby has them before we
# start mutating.  Use FileOps CREATE (WAL-logged, standby-reproduced) and
# FileOps MKDIR (WAL-logged) rather than raw open(), so the fixture itself
# replicates.  Paths are relative to PGDATA.
$primary->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_create('div_delete_me.dat', 384);   -- 0600
SELECT test_fileops_create('div_rename_src.dat', 384);
SELECT test_fileops_mkdir('div_rmdir_me', 448);         -- 0700
COMMIT;
));

# Take the base backup AFTER the fixture exists so init_from_backup gives the
# standby the fixture files, then stream the mutations.
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('fileops_div_standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

my $standby_dir = $standby->data_dir;

$primary->wait_for_catchup($standby);

# Sanity: the fixture replicated to the standby.
ok(-e "$standby_dir/div_delete_me.dat", 'fixture: delete target on standby');
ok(-e "$standby_dir/div_rename_src.dat", 'fixture: rename source on standby');
ok(-d "$standby_dir/div_rmdir_me", 'fixture: rmdir target on standby');

# Perform the three deferred destructive ops in a committed transaction on the
# primary.  Each goes through the FileOps deferred path (at_commit=true) and,
# with the fix, is WAL-logged at commit-execution.
$primary->safe_psql('postgres', qq(
BEGIN;
SELECT test_fileops_delete('div_delete_me.dat');
SELECT test_fileops_rename('div_rename_src.dat', 'div_rename_dst.dat');
SELECT test_fileops_rmdir('div_rmdir_me');
COMMIT;
));

# Confirm the primary applied them (the op ran in FileOpsDoPendingOps).
ok(!-e "$primary_dir/div_delete_me.dat", 'primary: delete applied');
ok(!-e "$primary_dir/div_rename_src.dat", 'primary: rename source gone');
ok(-e "$primary_dir/div_rename_dst.dat", 'primary: rename dest present');
ok(!-d "$primary_dir/div_rmdir_me", 'primary: rmdir applied');

$primary->wait_for_catchup($standby);

# The diff the reviewer asked for: the standby's filesystem must MATCH the
# primary's.  Pre-fix (no-op redo) the standby still has the original file /
# dir and lacks the renamed file -- these four assertions all fail, which is
# the non-vacuity proof.  Post-fix they pass.
ok(!-e "$standby_dir/div_delete_me.dat",
	'deferred DELETE reproduced on standby (file gone)');
ok(!-e "$standby_dir/div_rename_src.dat",
	'deferred RENAME reproduced on standby (old name gone)');
ok(-e "$standby_dir/div_rename_dst.dat",
	'deferred RENAME reproduced on standby (new name present)');
ok(!-d "$standby_dir/div_rmdir_me",
	'deferred RMDIR reproduced on standby (dir gone)');

# Idempotent redo across a standby restart: replaying the same records again
# after a crash must not error or diverge.
$standby->stop('immediate');
$standby->start;
$primary->safe_psql('postgres', 'SELECT txid_current()');
$primary->wait_for_catchup($standby);
ok(!-e "$standby_dir/div_delete_me.dat",
	'standby end state stable after restart (delete)');
ok(-e "$standby_dir/div_rename_dst.dat",
	'standby end state stable after restart (rename)');
ok(!-d "$standby_dir/div_rmdir_me",
	'standby end state stable after restart (rmdir)');

$standby->stop;
$primary->stop;

done_testing();
