# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX visibility while a rollback is still pending.
#
# FLUX updates in place: the page holds the newest version and the older
# ones live in the per-backend UNDO log.  When a transaction aborts, its
# in-place images stay on the page until the UNDO is applied.  Small live
# rollbacks apply inline, but a rollback handed to an undo apply worker
# (undo_rollback_overflow_size) and every crash loser are reverted later.
# Until then, readers must treat the aborted writer as invisible and walk
# past its versions to the last committed one.
#
# We keep the undo apply worker from ever starting: max_worker_processes is
# exactly the number of static background workers (discard worker, UNDO
# worker, undo worker launcher), so the launcher can never register one.
# That gate depends on the static worker count, so the test checks it
# directly: no "too many background workers" at startup, no undo apply worker
# in pg_stat_activity, and the aborted images still on the page.  A change in
# the worker count makes the test fail instead of passing without testing
# anything.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_deferred');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_logical_replication_workers = 0
max_logical_revert_workers = 0
max_parallel_workers = 0
max_parallel_workers_per_gather = 0
# discard worker + UNDO worker + undo worker launcher: no room for an
# undo apply worker
max_worker_processes = 3
# every rollback request goes to the (starved) undo apply worker
undo_rollback_overflow_size = 0
));
$node->start;

if (!$node->check_extension('pageinspect'))
{
	plan skip_all => 'Extension pageinspect not installed';
}

unlike(
	slurp_file($node->logfile),
	qr/too many background workers/,
	'every static background worker registered');

# Row 1's aborted value is 16908801 = 0x01020201, whose bytes read the same
# in either byte order, so the raw page can be searched for it.
# The gate holds: no undo apply worker ran, and the aborted image is still
# on the page of table $tab.
sub gate_holds
{
	my ($label, $tab) = @_;

	is( $node->safe_psql(
			'postgres',
			q{SELECT count(*) FROM pg_stat_activity
			  WHERE backend_type = 'undo apply worker'}),
		'0',
		"$label: no undo apply worker");
	is( $node->safe_psql(
			'postgres',
			"SELECT position('\\x01020201'::bytea IN get_raw_page('$tab', 0)) > 0"
		),
		't',
		"$label: aborted image still on the page");
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE f (id int PRIMARY KEY, v int) USING flux;
INSERT INTO f SELECT g, g * 10 FROM generate_series(1, 10) g;
UPDATE f SET v = 31 WHERE id = 3;
});

my $seq = 'SET enable_indexscan = off; SET enable_bitmapscan = off; ';
my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';

sub check_rows
{
	my ($label, %expect) = @_;

	is($node->safe_psql('postgres', $seq . 'SELECT count(*) FROM f'),
		'10', "$label: seqscan count unchanged");
	foreach my $id (sort keys %expect)
	{
		is( $node->safe_psql(
				'postgres', $seq . "SELECT v FROM f WHERE id = $id"),
			$expect{$id},
			"$label: seqscan sees committed value of row $id");
		is( $node->safe_psql(
				'postgres', $idx . "SELECT v FROM f WHERE id = $id"),
			$expect{$id},
			"$label: index scan sees committed value of row $id");
	}
}

# ---------------------------------------------------------------------------
# Live aborts whose rollback is deferred to the undo apply worker.
# ---------------------------------------------------------------------------
$node->safe_psql(
	'postgres', q{
BEGIN; UPDATE f SET v = 16908801 WHERE id = 1; ROLLBACK;
BEGIN; DELETE FROM f WHERE id = 2; ROLLBACK;
BEGIN; UPDATE f SET v = 32 WHERE id = 3; UPDATE f SET v = 33 WHERE id = 3; ROLLBACK;
});

gate_holds('deferred rollback', 'f');
check_rows('deferred rollback', 1 => '10', 2 => '20', 3 => '31');

# ---------------------------------------------------------------------------
# Crash with an in-place UPDATE in flight.
# ---------------------------------------------------------------------------
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('UPDATE f SET v = 4000 WHERE id = 4');

# Write the loser's page images out, so recovery does not have to redo the
# in-place UPDATEs (the CAS redo path is tested separately) and the pages
# hold the aborted and in-flight images at restart.
$node->safe_psql('postgres', 'CHECKPOINT');

$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;

gate_holds('crash loser', 'f');
check_rows('crash loser', 1 => '10', 2 => '20', 3 => '31', 4 => '40');

# ---------------------------------------------------------------------------
# Crash with an in-place UPDATE in flight, and no checkpoint: recovery has to
# redo the UPDATE (same-size CAS fast path), including the version-chain
# pointer it stamped.  Row 5 has never been updated, so its old t_verptr is
# invalid.
# ---------------------------------------------------------------------------
$node->append_conf('postgresql.conf', 'wal_consistency_checking = flux');
$node->restart;

# Take the page's first full-page image since the checkpoint on another
# row.  Otherwise row 5's UPDATE record would carry that FPI, redo would
# restore the page from it, and the record's byte-diff (the part that was
# missing t_verptr) would never be replayed.
$node->safe_psql('postgres', 'UPDATE f SET v = 61 WHERE id = 6');

$bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('UPDATE f SET v = 5000 WHERE id = 5');

# A commit flushes the WAL written before it, including the UPDATE above.
$node->safe_psql('postgres', 'CREATE TABLE flush_marker (x int)');

$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;

check_rows('crash loser, redo', 4 => '40', 5 => '50', 6 => '61');
unlike(
	slurp_file($node->logfile),
	qr/inconsistent page found/,
	'FLUX redo reproduces the primary page');

# ---------------------------------------------------------------------------
# Writers that meet an aborted, not-yet-reverted version must not depend on
# the undo apply worker: they revert that version themselves ("helping") and
# then proceed against the last committed version.
# ---------------------------------------------------------------------------
$node->safe_psql(
	'postgres', q{
CREATE TABLE h (id int PRIMARY KEY, v int, k int) USING flux;
CREATE INDEX h_k ON h (k);
INSERT INTO h SELECT g, g * 10, g FROM generate_series(1, 8) g;
BEGIN;
UPDATE h SET v = 16908801 WHERE id = 1;
UPDATE h SET v = v + 1000 WHERE id IN (2, 3, 4);
UPDATE h SET v = v + 1000 WHERE id = 4;
UPDATE h SET k = k + 100 WHERE id = 5;
DELETE FROM h WHERE id IN (6, 7);
ROLLBACK;
});
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_stat_activity
		  WHERE backend_type = 'undo apply worker'}),
	'0',
	'helping: no undo apply worker');
gate_holds('helping', 'h');

is( $node->safe_psql(
		'postgres', 'UPDATE h SET v = v + 1 WHERE id = 1 RETURNING v'),
	'11',
	'helping: UPDATE over an aborted in-place UPDATE');
is($node->safe_psql('postgres', 'DELETE FROM h WHERE id = 2 RETURNING v'),
	'20', 'helping: DELETE over an aborted in-place UPDATE');
is( $node->safe_psql(
		'postgres', 'BEGIN; SELECT v FROM h WHERE id = 3 FOR UPDATE; COMMIT;'
	),
	'30',
	'helping: SELECT FOR UPDATE over an aborted in-place UPDATE');
is( $node->safe_psql(
		'postgres', 'UPDATE h SET v = v + 1 WHERE id = 4 RETURNING v'),
	'41',
	'helping: UPDATE over a chain of two aborted in-place UPDATEs');
is( $node->safe_psql(
		'postgres', 'UPDATE h SET v = v + 1 WHERE k = 5 RETURNING v'),
	'51',
	'helping: UPDATE through the index over an aborted key UPDATE');
is( $node->safe_psql(
		'postgres', 'UPDATE h SET v = v + 1 WHERE id = 6 RETURNING v'),
	'61',
	'helping: UPDATE over an aborted DELETE');
is($node->safe_psql('postgres', 'DELETE FROM h WHERE id = 7 RETURNING v'),
	'70', 'helping: DELETE over an aborted DELETE');
is( $node->safe_psql(
		'postgres', $idx . 'SELECT count(*) FROM h WHERE k = 105'),
	'0',
	'helping: the aborted key UPDATE left no live index entry');

# Now let the undo apply worker run.  It must not clobber what the writers
# committed on top of the versions they reverted: each record a writer
# already reverted must be skipped by the idempotency key (t_verptr no
# longer points at it).  The log at DEBUG2 counts those skips.
$node->append_conf('postgresql.conf',
	"max_worker_processes = 8\nlog_min_messages = debug2");
my $log_offset = -s $node->logfile;
$node->restart;

# The worker restores table f's deferred rollbacks: wait until row 1's
# aborted image is gone from the page.
$node->poll_query_until('postgres',
	q{SELECT position('\\x01020201'::bytea IN get_raw_page('f', 0)) = 0})
  or die 'undo apply worker did not run';
$node->poll_query_until(
	'postgres',
	q{SELECT count(*) = 0 FROM pg_stat_activity
	  WHERE backend_type = 'undo apply worker'}
) or die 'undo apply worker did not finish';

# The worker did run (table f's images are restored, checked above), and for
# table h it found that writers had already reverted the aborted versions:
# it skipped those records by the idempotency key.
# Writers reverted 8 of h's records (rows 1-7, row 4 twice); f's records
# were all still current.  So the worker skips at least those 8 (it may
# meet a record twice if a request is retried).
my $wlog = substr(slurp_file($node->logfile), $log_offset);
my $skips = () =
  $wlog =~
  /undo apply worker.*FLUX UNDO: record at \d+ for .* was already undone/g;
cmp_ok($skips, '>=', 8,
	'helping: the undo apply worker skipped every record a writer reverted');
unlike($wlog, qr/cannot restore/, 'helping: no failed restore');
$node->append_conf('postgresql.conf', 'log_min_messages = warning');
$node->reload;

is( $node->safe_psql(
		'postgres',
		$seq
		  . q{SELECT string_agg(id || ':' || v || ':' || k, ',' ORDER BY id) FROM h}
	),
	'1:11:1,3:30:3,4:41:4,5:51:5,6:61:6,8:80:8',
	'helping: undo apply worker left the helped rows alone');
is( $node->safe_psql(
		'postgres',
		$idx
		  . q{SELECT string_agg(id::text, ',' ORDER BY id) FROM h WHERE k < 100}
	),
	'1,3,4,5,6,8',
	'helping: index agrees with the table');
check_rows(
	'after undo apply worker',
	1 => '10',
	2 => '20',
	3 => '31',
	4 => '40',
	5 => '50',
	6 => '61');

$node->stop;
done_testing();
