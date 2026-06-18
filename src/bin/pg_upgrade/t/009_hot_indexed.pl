# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade must preserve HOT-indexed on-disk state.  A relation that has
# accumulated HOT-indexed chains, including a value cycled away and back
# (ABA), an out-of-line (TOAST) indexed column, and chains collapsed to
# xid-free forwarding stubs by VACUUM, must come through an upgrade with its
# data intact, its indexes structurally sound, and its chains still scanning
# correctly.  pg_upgrade transfers heap and index files verbatim, so this is
# really a check that the new-state bits (HEAP_INDEXED_UPDATED, collapse stubs)
# are not rejected by pg_upgrade's checks and stay correct on the new cluster.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

my $oldnode = PostgreSQL::Test::Cluster->new('old_node');
$oldnode->init;
$oldnode->start;

# Build a relation with several secondary indexes so single-column updates
# stay HOT-indexed, then exercise the cases that produce interesting on-disk
# state.
$oldnode->safe_psql('postgres', q{
	CREATE EXTENSION amcheck;
	CREATE TABLE hi (id int PRIMARY KEY, k int, v int, big text)
	  WITH (fillfactor = 50);
	ALTER TABLE hi ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE INDEX hi_k ON hi (k);
	CREATE INDEX hi_v ON hi (v);
	CREATE INDEX hi_big ON hi (big);
	INSERT INTO hi SELECT g, g, g * 10, repeat(chr(64 + g), 2000)
	  FROM generate_series(1, 20) g;
});

# Interleave updates of different indexed columns on the same rows.  A member
# that changed a column not changed again by a later hop survives VACUUM as a
# collapse stub; the rest are reclaimed.  Row 1 additionally cycles k away and
# back (ABA), and row 2 rewrites its toasted indexed column.
$oldnode->safe_psql('postgres', q{
	UPDATE hi SET k = k + 100 WHERE id <= 10;   -- changes k
	UPDATE hi SET v = v + 1   WHERE id <= 10;    -- changes v (survives as stub)
	UPDATE hi SET k = k - 100 WHERE id <= 10;    -- k back to original (ABA)
	UPDATE hi SET big = repeat('Z', 2000) WHERE id = 2;
});
# Collapse dead chain members to stubs.
$oldnode->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP off) hi');

# The pre-upgrade state must already be self-consistent.
is( $oldnode->safe_psql('postgres',
		q{SELECT count(*) FROM verify_heapam('hi')}),
	'0', 'pre-upgrade heap is consistent');

# Snapshot the data we will compare after the upgrade.
my $expect = $oldnode->safe_psql('postgres',
	q{SELECT id, k, v, length(big) FROM hi ORDER BY id});

$oldnode->stop;

# New cluster, same version.
my $newnode = PostgreSQL::Test::Cluster->new('new_node');
$newnode->init;

my $oldbindir = $oldnode->config_data('--bindir');
my $newbindir = $newnode->config_data('--bindir');

# Run pg_upgrade from a writable directory (matches 002_pg_upgrade).
chdir ${PostgreSQL::Test::Utils::tmp_check};

command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		$mode,
	],
	'run of pg_upgrade for HOT-indexed relation');

$newnode->start;

# Data survived intact.
my $got = $newnode->safe_psql('postgres',
	q{SELECT id, k, v, length(big) FROM hi ORDER BY id});
is($got, $expect, 'HOT-indexed table data preserved across pg_upgrade');

# Heap and indexes are structurally sound on the new cluster.
is( $newnode->safe_psql('postgres',
		q{SELECT count(*) FROM verify_heapam('hi')}),
	'0', 'post-upgrade heap is consistent (collapse stubs recognised)');
is( $newnode->safe_psql('postgres', q{
		SELECT count(*) FROM (
			SELECT bt_index_check(c.oid)
			FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid
			WHERE i.indrelid = 'hi'::regclass) s}),
	'4', 'post-upgrade indexes pass bt_index_check');

# The ABA chain on row 1 still scans correctly through a forced index scan:
# k=1 returns exactly the one live row, and its superseded value is gone.
is( $newnode->safe_psql('postgres', q{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM hi WHERE k = 1}),
	'1', 'post-upgrade index scan returns the ABA row once');
is( $newnode->safe_psql('postgres', q{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM hi WHERE k = 101}),
	'0', 'post-upgrade index scan drops the superseded value');

$newnode->stop;

done_testing();
