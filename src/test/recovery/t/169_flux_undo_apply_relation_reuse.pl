
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# The FLUX UNDO apply path keeps one relation open across the records of a
# single top-level rollback instead of reopening it per record (see
# flux_undo_open_rel in flux_undo.c).  Reopening per record is O(records x
# segments) under RELCACHE_FORCE_RELEASE with small segment files -- an undo
# apply worker then never finishes a large rollback, and the non-permanent
# undo is never discarded (recovery/103 under --with-segsize-blocks=6).  The
# perf cliff itself only shows with many segment files, which a default
# RELSEG_SIZE build cannot reach; this test instead exercises the cache's
# LIFECYCLE correctness, which any build runs, so meson CI catches a
# regression in it:
#
#   - a worker rollback that spans TWO relations (the cache swaps relations),
#   - a rollback whose chain crosses a TRUNCATE in the aborted transaction
#     (the cached relation's file stops matching the record: the cache must
#     drop and re-open, not reapply to the wrong file),
#   - a worker rollback that an injection point makes fail partway and the
#     worker retries (the cache must not leak a relcache reference or lock
#     across the retry).
#
# In every case the undo is fully discarded, the rows are what the committed
# statements left, and no relcache-reference-leak WARNING, assertion failure
# or PANIC appears in the server log.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('undo_rel_reuse');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
shared_preload_libraries = 'injection_points'
# Hand every rollback to an undo apply worker, the path the cache serves.
undo_rollback_overflow_size = 0
# Section 3 waits for the worker's DEBUG1 report of a failed attempt.
log_min_messages = debug1
));
$node->start;

my $have_inj = $node->check_extension('injection_points');

# A permanent FLUX table changed first, so each session holds a permanent
# undo log whose meta precedes the checkpoint (as recovery/103 does).
$node->safe_psql('postgres',
	'CREATE TABLE p (v int) USING flux; INSERT INTO p VALUES (0)');

# All of a session's non-permanent (here: unlogged) undo has been discarded:
# the discard worker cannot get past a rollback that has not been applied, so
# this only reaches true once every worker rollback of this session's undo is
# complete.
my $undo_discarded = q(
SELECT coalesce(bool_and(discard = insert), true) FROM pg_stat_get_undo_logs()
 WHERE persistence = 'unlogged');

my $logstart = -s $node->logfile;

# ---------------------------------------------------------------------------
# 1. A worker rollback that spans two relations: the apply cache swaps from
#    one relation to the other within one execute_undo_actions().
# ---------------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE UNLOGGED TABLE a (id int, v text) USING flux;
CREATE UNLOGGED TABLE b (id int, v text) USING flux;
INSERT INTO a SELECT g, 'a' || g FROM generate_series(1, 5000) g;
INSERT INTO b SELECT g, 'b' || g FROM generate_series(1, 5000) g;
));
# One aborted transaction that updates BOTH tables, so its undo chain names
# two relations.  overflow_size = 0 hands it to an undo apply worker.
$node->safe_psql(
	'postgres', q(
BEGIN;
UPDATE a SET v = v || 'x';
UPDATE b SET v = v || 'x';
ROLLBACK;
));
$node->poll_query_until('postgres', $undo_discarded)
  or die 'two-relation rollback: undo not discarded';
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM a WHERE v = 'a' || id"),
	'5000', 'two-relation rollback: a restored');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM b WHERE v = 'b' || id"),
	'5000', 'two-relation rollback: b restored');

# ---------------------------------------------------------------------------
# 2. A rollback whose chain crosses a TRUNCATE inside the aborted
#    transaction: the pre-TRUNCATE records name a file the relation no longer
#    uses, so the cached relation stops matching and must be dropped and
#    re-opened (the relfilenode re-check in flux_undo_apply).
# ---------------------------------------------------------------------------
$node->safe_psql(
	'postgres', q(
CREATE UNLOGGED TABLE t (id int, v text) USING flux;
INSERT INTO t SELECT g, 'orig' || g FROM generate_series(1, 5000) g;
));
$node->safe_psql(
	'postgres', q(
BEGIN;
UPDATE t SET v = 'before';
TRUNCATE t;
INSERT INTO t SELECT g, 'after' || g FROM generate_series(1, 3000) g;
UPDATE t SET v = v || 'y';
ROLLBACK;
));
$node->poll_query_until('postgres', $undo_discarded)
  or die 'truncate-crossing rollback: undo not discarded';
# ROLLBACK undoes the whole transaction, TRUNCATE included: t is back to its
# original 5000 rows.
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE v = 'orig' || id"),
	'5000', 'truncate-crossing rollback: original rows restored');

# ---------------------------------------------------------------------------
# 3. A worker rollback that fails partway (injection point) and is retried.
#    The cache must release its relation on the failed attempt (the worker
#    aborts its transaction, firing the xact callback) and not leak a
#    reference or lock across the retry.
# ---------------------------------------------------------------------------
SKIP:
{
	skip 'injection_points not installed', 1 unless $have_inj;

	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql(
		'postgres', q(
CREATE UNLOGGED TABLE r (id int, v text) USING flux;
INSERT INTO r SELECT g, 'orig' || g FROM generate_series(1, 5000) g;
));
	# pbu-undo-apply-skip is only tested with IS_INJECTION_POINT_ATTACHED(),
	# never run, so the action is immaterial; merely being attached forces
	# every record to UNDO_APPLY_SKIPPED, leaving the rollback incomplete so
	# the worker retries from the start.  Detaching it lets a later retry
	# succeed.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('pbu-undo-apply-skip', 'notice')");
	my $off = -s $node->logfile;
	$node->safe_psql(
		'postgres', q(
BEGIN;
UPDATE r SET v = 'inflight';
ROLLBACK;
));
	# Let the first attempt fail, then let a retry succeed.  Detach on the
	# first failure rather than after a fixed time: the worker gives up after
	# a few attempts a second apart, which a slow host can use up first.
	$node->wait_for_log(qr/Undo action pushed\(error\)/, $off);
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-apply-skip')");
	$node->poll_query_until('postgres', $undo_discarded)
	  or die 'retried rollback: undo not discarded';
	is( $node->safe_psql('postgres',
			"SELECT count(*) FROM r WHERE v = 'orig' || id"),
		'5000', 'retried rollback: rows restored after retry');
}

# No relcache reference leak, assertion failure or PANIC in the whole run: a
# dangling cached relation or a missed release would surface as one of these.
my $log = slurp_file($node->logfile, $logstart);
unlike($log, qr/relcache reference leak/, 'no relcache reference leak');
unlike($log, qr/refcount|ResourceOwner .* leak/i, 'no resource owner leak');
unlike($log, qr/TRAP|PANIC|assertion/i, 'no assertion failure or PANIC');

$node->stop;
done_testing();
