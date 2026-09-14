
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Undo redo must not rewrite an undo page that already holds a later change.
#
# FLUX redo re-runs the undo insert carried on each forward record, while
# XLOG_UNDO_AMEND_RECORD patches an inserted record under the page LSN.  An
# indexed key that goes A -> B -> A revives the (A, TID) entry the first
# UPDATE delete-marked, and says so in the second UPDATE's undo record
# (FLUX_UNDO_INDEX_REVIVED) by such an amendment.  Without full-page writes,
# an undo page written out after the amendment carries the amendment's LSN,
# and recovery from a redo pointer before the insert must neither re-insert
# the record over it nor, of course, skip the amendment and lose it.  If it
# did, reverting the second UPDATE would remove (A, TID) instead of
# delete-marking it again, and reverting the first would find nothing to
# make live: the row would drop out of the index.
#
# The undo apply worker cannot start (see 076_flux_deferred_rollback.pl), so
# the crash loser is reverted by the writer that meets it ("helping").

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
full_page_writes = off
max_logical_replication_workers = 0
max_logical_revert_workers = 0
max_parallel_workers = 0
max_parallel_workers_per_gather = 0
max_worker_processes = 3
undo_rollback_overflow_size = 0
));
$node->start;

if (   !$node->check_extension('pg_buffercache')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'pg_buffercache or amcheck not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pg_buffercache;
CREATE EXTENSION amcheck;
CREATE TABLE f (id int PRIMARY KEY, k int, v int) USING flux;
CREATE INDEX f_k ON f (k);
INSERT INTO f SELECT g, g, g * 10 FROM generate_series(1, 10) g;
CHECKPOINT;
});

# The redo pointer is now before the loser's undo inserts.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('UPDATE f SET k = 100, v = 11 WHERE id = 1');
$bg->query_safe('UPDATE f SET k = 1 WHERE id = 1');

# Write the undo pages out, carrying the amendment's LSN, without a
# checkpoint.  The undo logs live in database 9 (UndoLogDatabaseOid).
cmp_ok(
	$node->safe_psql(
		'postgres', q{
SELECT count(*) FROM pg_buffercache b
WHERE b.reldatabase = 9 AND (pg_buffercache_evict(b.bufferid)).buffer_flushed}
	),
	'>',
	0,
	'undo pages written out');

$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;

my $seq = 'SET enable_indexscan = off; SET enable_bitmapscan = off; ';
my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';

is( $node->safe_psql(
		'postgres',
		$seq . 'UPDATE f SET v = v + 1 WHERE id = 1 RETURNING k, v'),
	'1|11',
	'a writer reverts the loser and updates the committed version');
is($node->safe_psql('postgres', $idx . 'SELECT v FROM f WHERE k = 1'),
	'11', 'index scan finds the row under its key');
is( $node->safe_psql(
		'postgres', $idx . 'SELECT count(*) FROM f WHERE k = 100'),
	'0',
	'no live entry under the aborted key');
is( $node->safe_psql('postgres', $idx . 'SELECT count(*) FROM f WHERE k > 0'),
	'10',
	'index scan count matches the table');
my ($ret, $out, $err) =
  $node->psql('postgres', "SELECT bt_index_check('f_k', true)");
is($ret . $err, '0', 'bt_index_check(f_k, heapallindexed)');

$node->stop;
done_testing();
