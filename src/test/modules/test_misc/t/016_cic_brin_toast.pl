# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that BRIN fetches external values before resetting its snapshot.
# Pause a concurrent build after it summarizes page 0, then delete that
# page's row.  A retained TOAST pointer would fail on the next comparison
# when the build's newer snapshot allows the TOAST data to be pruned.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
# Nothing else may hold back the horizon.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

# One row per page whatever the block size, with values stored out of line.
$node->safe_psql(
	'postgres', q[
CREATE EXTENSION injection_points;
SELECT current_setting('block_size')::int AS bs,
       current_setting('block_size')::int / 2 AS pad \gset
CREATE TABLE brin_toast (i int, t text, pad char(:pad));
ALTER TABLE brin_toast ALTER COLUMN t SET STORAGE EXTERNAL,
                       ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO brin_toast
    SELECT g, repeat(chr(64 + g), :bs * 4), 'x' FROM generate_series(1, 2) g;
]);

my $cic = $node->background_psql('postgres', on_error_stop => 0);
$cic->query_safe(
	q[
SET concurrent_index_reset_snapshot_interval = 0;
SELECT injection_points_set_local();
SELECT injection_points_attach('heap_reset_scan_snapshot_swap', 'wait');
]);
$cic->query_until(
	qr/starting/, q[
\echo starting
CREATE INDEX CONCURRENTLY brin_toast_idx ON brin_toast USING brin (t);
]);

# The build stops before each page's snapshot reset.  Commit a transaction
# at the first stop so a changed xmin identifies the second stop.
my $xmin = wait_for_stop($node, 'NULL');
$node->safe_psql(
	'postgres', q[
SELECT pg_current_xact_id();
SELECT injection_points_wakeup('heap_reset_scan_snapshot_swap');
]);

# Detach before waking up, so the build runs to the end.
wait_for_stop($node, "'$xmin'::xid");
$node->safe_psql(
	'postgres', q[
DELETE FROM brin_toast WHERE i = 1;
SELECT injection_points_detach('heap_reset_scan_snapshot_swap');
SELECT injection_points_wakeup('heap_reset_scan_snapshot_swap');
]);
$cic->quit;

is( $node->safe_psql(
		'postgres',
		q[SELECT indisvalid FROM pg_index WHERE indexrelid = 'brin_toast_idx'::regclass]
	),
	't',
	'build survived the delete of a summarized row');

# Verify that the index remains usable after the delete.
like(
	$node->safe_psql(
		'postgres', q[
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM brin_toast WHERE t < 'C';
]),
	qr/Bitmap Index Scan on brin_toast_idx/,
	'index is used');
is( $node->safe_psql(
		'postgres', q[
SET enable_seqscan = off;
SELECT count(*) FROM brin_toast WHERE t < 'C';
]),
	'1',
	'index scan finds the remaining row');

done_testing();

# Wait until the build sleeps at the reset point with an xmin other than
# the given one, and return its xmin.
sub wait_for_stop
{
	my ($node, $prev_xmin) = @_;

	$node->poll_query_until(
		'postgres', qq[
SELECT count(*) > 0 FROM pg_stat_activity
WHERE wait_event = 'heap_reset_scan_snapshot_swap'
  AND backend_xmin IS DISTINCT FROM $prev_xmin
]) or die "timed out waiting for the build to stop at a snapshot reset";

	return $node->safe_psql(
		'postgres', q[
SELECT backend_xmin FROM pg_stat_activity
WHERE wait_event = 'heap_reset_scan_snapshot_swap'
]);
}
