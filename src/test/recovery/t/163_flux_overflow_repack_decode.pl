
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# REPACK (CONCURRENTLY) of a FLUX table with a wide native-overflow value,
# updated concurrently, must not self-lock a buffer.
#
# REPACK (CONCURRENTLY) turns on logical decoding (XLogLogicalInfo) even at
# wal_level=replica, so a concurrent UPDATE on the table reads as logically
# logged and prepares a logical-decoding image of the OLD row.  That old row's
# wide column was written when the relation was NOT logically logged, so it
# lives in FLUX-native overflow, and the deform that builds the image resolves
# it through FluxFetchOverflowColumn while the UPDATE already holds the data
# page's buffer lock.  A native overflow chunk can share that very page, so the
# fetch would re-lock a buffer it already holds -- a self-lock that aborts the
# backend (Assert lockmode == BUFFER_LOCK_UNLOCK in bufmgr).  The held-aware
# fetch must read the chunk from the pinned buffer instead.
#
# RED before "flux: make overflow fetch held-aware to avoid a buffer self-lock"
# (the UPDATE crashes the backend and takes down the cluster); GREEN after, with
# the row's content intact and the repack complete.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('repack_overflow');
$node->init;
# wal_level=replica: at INSERT time the relation is NOT logically logged, so the
# wide value goes to FLUX-native overflow (not heap TOAST).  REPACK
# (CONCURRENTLY) flips XLogLogicalInfo on for the concurrent UPDATE below.
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_level = replica
shared_preload_libraries = 'injection_points'
max_wal_senders = 4
max_replication_slots = 4
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE t (id int PRIMARY KEY, k int, w text) USING flux
	WITH (fillfactor = 100, autovacuum_enabled = off);
ALTER TABLE t ALTER COLUMN w SET STORAGE EXTERNAL;
CREATE INDEX t_k ON t (k);
-- rows 1-5 hold a ~3.2 kB wide value in FLUX-native overflow
INSERT INTO t SELECT g, g,
	CASE WHEN g <= 5 THEN repeat(md5(g::text), 100) END
	FROM generate_series(1, 120) g;
});

# Hold REPACK (CONCURRENTLY) at repack-concurrently-before-lock, after the copy
# and index builds, before the catch-up.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('repack-concurrently-before-lock', 'wait')"
);
my $repack = $node->background_psql('postgres', on_error_stop => 0);
$repack->query_until(qr/start/, "\\echo start\nREPACK (CONCURRENTLY) t;\n");
$node->wait_for_event('client backend', 'repack-concurrently-before-lock');

# The concurrent UPDATE of the wide column: its logical-decoding image deforms
# the OLD row, whose wide value is in native overflow -- the crash trigger.
$node->safe_psql('postgres',
	"UPDATE t SET w = repeat(md5('upd'), 100) WHERE id = 1");

# If the self-lock fired, the backend aborted and the cluster is in recovery;
# this query would fail.  It must report the updated value intact.
is( $node->safe_psql(
		'postgres', "SELECT w = repeat(md5('upd'), 100) FROM t WHERE id = 1"),
	't',
	'UPDATE of a wide native-overflow value under REPACK (CONCURRENTLY) did not self-lock');

# Let REPACK finish and confirm it completed (replayed the concurrent UPDATE).
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('repack-concurrently-before-lock')");
$repack->query_safe("SELECT 1");
$repack->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('repack-concurrently-before-lock')");

is( $node->safe_psql(
		'postgres', q{
SELECT count(*),
	   count(*) FILTER (WHERE id = 1 AND w = repeat(md5('upd'), 100)),
	   count(*) FILTER (WHERE id BETWEEN 2 AND 5
						AND w = repeat(md5(id::text), 100))
FROM t}),
	'120|1|4',
	'after REPACK: row count and every wide value intact');

ok( !$node->log_contains('BUFFER_LOCK_UNLOCK'),
	'no buffer self-lock assertion in the log');

$node->stop;
done_testing();
