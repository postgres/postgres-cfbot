# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify snapshot resets during the validation phase of CREATE INDEX
# CONCURRENTLY:
# - round 1 attaches an error to the unconditional
#   "validate-index-snapshot-reset" injection point and expects CIC to
#   fail, proving that resets do happen;
# - round 2 attaches an error to the "validate-index-xmin-not-advanced"
#   injection point and expects CIC to succeed, proving that every reset
#   fully advances the backend's xmin; a notice on the reset point
#   confirms that resets did happen during that run.

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
# Injection point notices must reach the server log (round 2 checks them).
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
	my ($marker, $first_new_row) = @_;

	$blocker->query_safe(q[SELECT pg_advisory_lock(4971);]);

	$cic->query_until(
		qr/$marker/, qq[
\\echo $marker
SET debug_cic_validate_snapshot_interval = 1;
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
	$node->safe_psql('postgres',
		q[INSERT INTO tbl SELECT generate_series(100001, 200000);]);

	$blocker->query_safe(q[SELECT pg_advisory_unlock(4971);]);
}

# Round 1: snapshot resets must happen during validation.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-snapshot-reset', 'error');]);

run_cic_with_candidates('starting_cic1', 100001);

$node->wait_for_log(
	qr/error triggered for injection point validate-index-snapshot-reset/);
ok(1, 'snapshot reset happened during CIC validation');

# The failed CIC left an invalid index behind; dropping it must take the
# auxiliary index with it.
$node->poll_query_until(
	'postgres', q[
SELECT NOT EXISTS (SELECT FROM pg_stat_activity
	WHERE query LIKE 'CREATE INDEX CONCURRENTLY%' AND state = 'active')])
  or die "timed out waiting for CIC to fail";
$node->safe_psql('postgres', q[DROP INDEX idx;]);
is( $node->safe_psql(
		'postgres',
		q[SELECT count(*) FROM pg_class WHERE relname IN ('idx', 'idx_ccaux')]),
	'0',
	'failed CIC cleaned up after DROP INDEX');

$node->safe_psql('postgres',
	q[SELECT injection_points_detach('validate-index-snapshot-reset');]);

# Round 2: every reset must fully advance xmin.  The reset point now only
# raises a notice, both to prove that resets do happen during this
# (successful) run and to let it complete.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-snapshot-reset', 'notice');]);
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('validate-index-xmin-not-advanced', 'error');]);

my $log_offset = -s $node->logfile;

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

eval { $blocker->quit };
eval { $cic->quit };

done_testing();
