# Copyright (c) 2026, PostgreSQL Global Development Group

# Check validation snapshot resets with and without STIR candidates, disabled
# resets, transaction isolation, and snapshots held by cursors.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->append_conf('postgresql.conf', 'fsync = off');
# Injection point notices must reach the server log.
$node->append_conf('postgresql.conf', 'log_min_messages = notice');
$node->start();

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck;');

$node->safe_psql(
	'postgres', q[
CREATE UNLOGGED TABLE tbl (i int);
INSERT INTO tbl SELECT generate_series(1, 1000);
-- The function lies about immutability so that the build scan of CIC can
-- be paused on an advisory lock held by another session.
CREATE FUNCTION indexpr(int) RETURNS int IMMUTABLE LANGUAGE plpgsql
AS $$ BEGIN PERFORM pg_advisory_xact_lock(4971); RETURN $1; END $$;
]);

my $blocker = $node->background_psql('postgres', on_error_stop => 0);
my $cic = $node->background_psql('postgres', on_error_stop => 0);

# Pause the CIC build scan on the first evaluation of the index
# expression, insert rows that will only be reachable through the
# auxiliary index (making the validation phase do real work), then let
# CIC finish.
sub run_cic_with_candidates
{
	my ($marker, $first_new_row, $interval, $isolation) = @_;
	$interval //= 0;
	$isolation //= 'read committed';

	$blocker->query_safe(q[SELECT pg_advisory_lock(4971);]);

	$cic->query_until(
		qr/$marker/, qq[
\\echo $marker
SET concurrent_index_reset_snapshot_interval = $interval;
SET default_transaction_isolation = '$isolation';
CREATE INDEX CONCURRENTLY idx ON tbl (indexpr(i));
]);

	# Wait for the build scan to block on the advisory lock.
	$node->poll_query_until(
		'postgres', q[
SELECT EXISTS (SELECT FROM pg_locks
	WHERE locktype = 'advisory' AND objid = 4971 AND NOT granted)])
	  or die "timed out waiting for CIC to block on the advisory lock";

	# These rows are seen neither by the build scan snapshot nor by the
	# target index (not ready yet), so the validation phase will have to
	# fetch and insert all of them.
	my $last_new_row = $first_new_row + 999;
	$node->safe_psql('postgres',
		"INSERT INTO tbl SELECT generate_series($first_new_row, $last_new_row);");

	$blocker->query_safe(q[SELECT pg_advisory_unlock(4971);]);
}

# An empty STIR skips the heap scan, but validation must still reset snapshots.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-snapshot-reset', 'error');]);

my ($ret, $stdout, $stderr) = $node->psql('postgres', q[
SET concurrent_index_reset_snapshot_interval = 0;
CREATE INDEX CONCURRENTLY idx ON tbl (indexpr(i));
]);
like($stderr,
	qr/error triggered for injection point validate-index-snapshot-reset/,
	'snapshot reset happens before the validation heap scan');

# The failed CIC left an invalid index behind; dropping it must take the
# auxiliary index with it.
$node->safe_psql('postgres', q[DROP INDEX idx;]);
is( $node->safe_psql(
		'postgres',
		q[SELECT count(*) FROM pg_class WHERE relname IN ('idx', 'idx_ccaux')]),
	'0',
	'failed CIC cleaned up after DROP INDEX');

$node->safe_psql('postgres',
	q[SELECT injection_points_detach('validate-index-snapshot-reset');]);

# Every reset must fully advance xmin.  The reset point now only
# raises a notice, both to prove that resets do happen during this
# (successful) run and to let it complete.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-snapshot-reset', 'notice');]);
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-xmin-not-advanced', 'error');]);

my $log_offset = -s $node->logfile;

# The early return for an empty STIR must also leave a complete index.
$node->safe_psql('postgres', q[
SET concurrent_index_reset_snapshot_interval = 0;
CREATE INDEX CONCURRENTLY idx ON tbl (indexpr(i));
SELECT bt_index_check('idx', heapallindexed => true);
]);
$node->wait_for_log(
	qr/notice triggered for injection point validate-index-snapshot-reset/,
	$log_offset);
ok(1, 'CIC with an empty STIR completes with snapshot resets');
$node->safe_psql('postgres', q[DROP INDEX idx;]);

$log_offset = -s $node->logfile;
run_cic_with_candidates('starting_cic2', 200001);

$node->poll_query_until(
	'postgres', q[
SELECT indisvalid FROM pg_index
	WHERE indexrelid = (SELECT oid FROM pg_class WHERE relname = 'idx')])
  or die "timed out waiting for CIC to complete";
ok(1, 'CIC completed with snapshot resets fully advancing xmin');

$node->wait_for_log(
	qr/notice triggered for injection point validate-index-snapshot-reset/,
	$log_offset);
ok(1, 'snapshot resets happened during the successful CIC');

# The index must contain everything despite the snapshot changes.
is( $node->safe_psql('postgres',
		q[SELECT bt_index_check('idx', heapallindexed => true)]),
	'', 'index is complete');

$node->safe_psql('postgres', q[DROP INDEX idx;]);
$node->safe_psql('postgres',
	q[SELECT injection_points_detach('validate-index-snapshot-reset');
SELECT injection_points_attach('validate-index-snapshot-reset', 'error');]);

# Disabled resets and transaction snapshots must keep a fixed snapshot.
my @fixed_cases = (
	['disabled', -1, 'read committed'],
	['repeatable_read', 0, 'repeatable read'],
	['serializable', 0, 'serializable']);
my $first_row = 300001;
for my $case (@fixed_cases)
{
	my ($name, $interval, $isolation) = @$case;
	run_cic_with_candidates("starting_$name", $first_row, $interval, $isolation);
	$node->poll_query_until('postgres',
		q[SELECT indisvalid FROM pg_index WHERE indexrelid = to_regclass('idx')])
	  or die "timed out waiting for $name CIC";
	is($node->safe_psql('postgres',
		q[SELECT bt_index_check('idx', heapallindexed => true)]),
		'', "$name: index is complete without resets");
	$node->safe_psql('postgres', q[DROP INDEX idx;]);
	$first_row += 100000;
}

# A cursor opened by an index expression keeps an extra snapshot reference.
$node->safe_psql('postgres',
	q[SELECT injection_points_detach('validate-index-snapshot-reset');
SELECT injection_points_attach('validate-index-snapshot-reset', 'notice');
SELECT injection_points_detach('validate-index-xmin-not-advanced');
SELECT injection_points_attach('validate-index-xmin-not-advanced', 'notice');
SELECT injection_points_attach('validation-cursor-open', 'wait');]);
$node->safe_psql('postgres', qq[
CREATE OR REPLACE FUNCTION indexpr(int) RETURNS int IMMUTABLE LANGUAGE plpgsql
AS \$\$
DECLARE c refcursor := 'validation_pin';
BEGIN
    PERFORM pg_advisory_xact_lock(4971);
    IF \$1 = $first_row THEN
        OPEN c FOR SELECT 1;
        PERFORM public.injection_points_run('validation-cursor-open');
    END IF;
    RETURN \$1;
END \$\$;
]);
$log_offset = -s $node->logfile;
run_cic_with_candidates('starting_pinned', $first_row);
$node->poll_query_until('postgres',
	q[SELECT EXISTS (SELECT FROM pg_stat_activity
		WHERE wait_event = 'validation-cursor-open')])
  or die 'timed out waiting for the validation cursor';
# Give the next snapshot a newer xmin than the cursor's snapshot.
$node->safe_psql('postgres', q[SELECT pg_current_xact_id();]);
$node->safe_psql('postgres',
	q[SELECT injection_points_wakeup('validation-cursor-open');]);
$node->poll_query_until('postgres',
	q[SELECT indisvalid FROM pg_index WHERE indexrelid = to_regclass('idx')])
  or die 'timed out waiting for CIC with a pinned snapshot';
$node->wait_for_log(
	qr/notice triggered for injection point validate-index-snapshot-reset/,
	$log_offset);
$node->wait_for_log(
	qr/notice triggered for injection point validate-index-xmin-not-advanced/,
	$log_offset);
ok(1, 'cursor keeps xmin pinned across a snapshot reset');
$node->safe_psql('postgres',
	q[SELECT injection_points_detach('validation-cursor-open');]);
is($node->safe_psql('postgres',
	q[SELECT bt_index_check('idx', heapallindexed => true)]),
	'', 'index is complete with a snapshot held by a cursor');

eval { $blocker->quit };
eval { $cic->quit };

done_testing();
