# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX UNDO apply that cannot restore a before-image must not report
# success.
#
# An in-place UPDATE never shrinks a tuple's slot, so a current record's
# before-image always fits.  If a restore cannot be done anyway,
# flux_undo_apply must return UNDO_APPLY_ERROR, not SUCCESS: the rollback
# request stays, and is retried once the cause is gone.  Before this, the
# restore was skipped with a WARNING and reported as done, so the aborted
# image was left on the page for good.  The injection point
# flux-undo-restore-no-room simulates the failure.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_x1');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$node->start;

if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('pageinspect'))
{
	plan skip_all => 'injection_points or pageinspect not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE TABLE f (id int PRIMARY KEY, v int) USING flux;
INSERT INTO f SELECT g, g * 10 FROM generate_series(1, 4) g;
SELECT injection_points_attach('flux-undo-restore-no-room', 'notice');
});

# 0x01020201 reads the same in either byte order, so the raw page can be
# searched for it.
my $on_page =
  q{SELECT position('\x01020201'::bytea IN get_raw_page('f', 0)) > 0};

# The rollback cannot restore the row.  It must not be marked done: the
# aborted image is still on the page, the transaction header still says
# "not applied", and the request is still queued.
#
# A small rollback is applied inline by the aborting backend; no undo apply
# worker takes part in it.  Count them from that backend: once it exits, it
# hands the rollback it could not finish to an undo apply worker.
my $start_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
my $workers = $node->safe_psql(
	'postgres', q{
BEGIN; UPDATE f SET v = 16908801 WHERE id = 1; ROLLBACK;
SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'undo apply worker';
});
$node->safe_psql('postgres', 'CHECKPOINT');    # flush the rollback's WAL
my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
is($node->safe_psql('postgres', $on_page),
	't', 'failed restore left the aborted image on the page');
is($workers, '0', 'rollback was applied inline, not by a worker');

# Marking the rollback complete writes XLOG_UNDO_APPLY_PROGRESS.  It must not
# be written for a rollback that left a record unapplied.
my ($stdout, $stderr);
IPC::Run::run [
	'pg_waldump',
	'--path' => $node->data_dir . '/pg_wal',
	'--start' => $start_lsn,
	'--end' => $end_lsn,
	'--rmgr' => 'UndoAction'
  ],
  '>' => \$stdout,
  '2>' => \$stderr;
like($stdout . $stderr, qr/rmgr: UndoAction|^$/s, 'pg_waldump ran');
unlike($stdout, qr/APPLY_PROGRESS/,
	'rollback with an unapplied record is not marked complete');

# Readers are not affected: the aborted writer is invisible.
is($node->safe_psql('postgres', 'SELECT v FROM f WHERE id = 1'),
	'10', 'reader sees the committed value');

# Once the cause is gone, the outstanding rollback is completed.  A writer
# running into the row reverts it itself (helping) and builds on the
# committed value; the queued request then finds nothing left to do.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('flux-undo-restore-no-room')");
is( $node->safe_psql(
		'postgres', 'UPDATE f SET v = v + 1 WHERE id = 1 RETURNING v'),
	'11',
	'writer builds on the committed value');
is($node->safe_psql('postgres', $on_page), 'f', 'aborted image is gone');

# The same failure inside ROLLBACK TO SAVEPOINT.  The record must not be
# marked applied: it stays due, so the transaction refuses to commit (which
# would keep the aborted image) and its rollback applies the record.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('flux-undo-restore-no-room', 'notice')");
my ($ret, $out, $err) = $node->psql(
	'postgres', q{
BEGIN;
SAVEPOINT s;
UPDATE f SET v = 16908801 WHERE id = 2;
ROLLBACK TO s;
SELECT injection_points_detach('flux-undo-restore-no-room');
COMMIT;
});
like(
	$err,
	qr/rollback to a savepoint was incomplete/,
	'commit after an incomplete ROLLBACK TO is refused');
is($node->safe_psql('postgres', $on_page),
	'f', 'the rollback that followed applied the unapplied record');
is( $node->safe_psql(
		'postgres', 'UPDATE f SET v = v + 1 WHERE id = 2 RETURNING v'),
	'21',
	'writer builds on the committed value');

$node->restart;
is( $node->safe_psql(
		'postgres',
		'SELECT string_agg(id || \':\' || v, \',\' ORDER BY id) FROM f'),
	'1:11,2:21,3:30,4:40',
	'no later apply clobbered the committed value');

$node->stop;
done_testing();
