
# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash-recovery coverage for HOT-indexed (HOT/SIU) chains.
#
# Build a HOT-indexed chain by repeatedly UPDATEing a single row,
# changing one indexed (non-PK) column each time.  Force a prune so the
# dead chain members collapse to LP_REDIRECT forwarders (with the live
# HOT-indexed version visible via pg_relation_hot_indexed_stats as
# n_hot_indexed > 0).  Crash-recover the primary with stop('immediate')
# so the collapsed chain comes back from WAL or from the FPI.  After
# restart, verify:
#
#   1. an index lookup walking the chain returns the live tuple,
#   2. pg_amcheck (verify_heapam) reports no errors on the relation,
#   3. VACUUM reclaims the collapsed chain (n_hot_indexed drops to 0).
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# Disable autovacuum to keep the chain shape stable up to the explicit
# prune we trigger below.
$node->append_conf('postgresql.conf', q{autovacuum = off
wal_consistency_checking = 'all'});
$node->start;

# amcheck (verify_heapam) is shipped as a contrib extension; we use it
# from SQL after the crash-restart cycle.
$node->safe_psql('postgres', q{CREATE EXTENSION amcheck});

# Wide-ish table: PK + four indexed columns plus a non-indexed payload
# so HOT-indexed updates have width to amortise.  fillfactor = 50 keeps
# free space on-page for HOT-indexed continuations.
$node->safe_psql('postgres', q{
	CREATE TABLE hi_recov (
		id      int PRIMARY KEY,
		c1      int,
		c2      int,
		c3      int,
		c4      int,
		payload text
	) WITH (fillfactor = 50);
	CREATE INDEX hi_recov_c1 ON hi_recov(c1);
	CREATE INDEX hi_recov_c2 ON hi_recov(c2);
	CREATE INDEX hi_recov_c3 ON hi_recov(c3);
	CREATE INDEX hi_recov_c4 ON hi_recov(c4);
	INSERT INTO hi_recov VALUES (1, 100, 200, 300, 400, 'payload');
});

# Build a HOT-indexed chain: five UPDATEs, each touching one indexed
# column.  Every UPDATE keeps the new version on-page and plants a fresh
# index entry because c1 is indexed and changed.  Use a SQL transaction-
# range loop so each UPDATE is its own xact (xmin/xmax distinct).
for my $i (1 .. 5)
{
	my $newval = 100 + $i;
	$node->safe_psql('postgres',
		"UPDATE hi_recov SET c1 = $newval WHERE id = 1");
}

my $pre_prune = $node->safe_psql('postgres',
	q{SELECT n_hot_indexed FROM pg_relation_hot_indexed_stats('hi_recov')});
cmp_ok($pre_prune, '>', 0,
	'HOT-indexed chain has at least one live HOT-indexed version before prune');

# Force a prune.  The chain has dead heap-only members from the early
# UPDATEs (their xmins are now committed and below the snapshot horizon).
# A SELECT under default isolation visits the page; under
# default_statistics_target etc. that's not enough on its own to trigger
# prune.  The reliable way to drive opportunistic prune is a query that
# exercises the heap_page_prune_opt path, which fires from an indexscan
# that finds the page non-all-visible.  Use a sequential scan plus a
# subsequent UPDATE that itself looks for free space (heap_update calls
# heap_page_prune_opt).
$node->safe_psql('postgres', q{
	SET enable_indexscan = off;
	SELECT count(*) FROM hi_recov;
	UPDATE hi_recov SET payload = 'pruned' WHERE id = 1;
});

# Read the chain state after the prune: the live HOT-indexed version
# remains while dead members collapse to LP_REDIRECT forwarders.
my $post_prune = $node->safe_psql('postgres',
	q{SELECT n_hot_indexed FROM pg_relation_hot_indexed_stats('hi_recov')});
cmp_ok($post_prune, '>', 0,
	'live HOT-indexed version survives opportunistic prune');

# Crash-restart.  stop('immediate') is the standard "kill -9" simulation
# used elsewhere in src/test/recovery/.  We intentionally do NOT issue a
# CHECKPOINT first: that would advance the redo point past the HOT-indexed
# UPDATE/prune records and leave nothing for recovery to replay.  Crashing
# without a checkpoint forces startup redo to reconstruct the collapsed
# chain from WAL, and wal_consistency_checking = 'all' (set above) compares
# each replayed page against its full-page image, catching any divergence
# between the write path and the redo path.
$node->stop('immediate');
$node->start;

# Chain walk via the indexed column on the live row returns the
#    correct (and only the correct) tuple.  c1 = 105 was the last
#    UPDATE, so the live tuple has c1 = 105 and c2..c4 unchanged.
my $live = $node->safe_psql('postgres', q{
	SET enable_seqscan = off;
	SELECT id, c1, c2, c3, c4, payload FROM hi_recov WHERE c1 = 105;
});
is($live, "1|105|200|300|400|pruned",
	'index lookup on chain returns the post-prune live tuple');

# Older c1 values are not reachable: every stale btree entry that
# chain-resolves across a HOT/SIU hop must be dropped by the read-side
# crossed-attribute bitmap.
my $stale_count = $node->safe_psql('postgres',
	q{SELECT count(*) FROM hi_recov WHERE c1 = 100});
is($stale_count, '0',
	'stale btree entries are filtered by the crossed-attribute bitmap');

# verify_heapam reports no errors on the relation (skip_option =
#    'all-frozen' is the default; we want to scan everything).
my $heapcheck = $node->safe_psql('postgres', q{
	SELECT count(*) FROM verify_heapam('hi_recov',
	                                   skip := 'none',
	                                   check_toast := false);
});
is($heapcheck, '0',
	'verify_heapam reports zero errors after crash recovery');

# Reclamation: after the live row is deleted, two VACUUM (FREEZE)
#    passes drive prune to revisit the page and reclaim the now-fully-dead
#    collapsed chain (the first removes the dead row's index entries and
#    reduces its LP; the second reclaims the unreferenced members and
#    re-points the redirect).  After that, n_hot_indexed must be zero.
$node->safe_psql('postgres', q{DELETE FROM hi_recov WHERE id = 1});
$node->safe_psql('postgres',
	q{VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) hi_recov});
$node->safe_psql('postgres',
	q{VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) hi_recov});
my $final = $node->safe_psql('postgres',
	q{SELECT n_hot_indexed FROM pg_relation_hot_indexed_stats('hi_recov')});
is($final, '0',
	'two VACUUM (FREEZE) passes after DELETE reclaim the chain post-recovery');

$node->stop;

done_testing();
