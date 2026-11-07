# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->start;
$node->safe_psql(
	'postgres', q[
CREATE ROLE progress_viewer;
CREATE TABLE progress_data AS SELECT generate_series(1, 64) AS i;
CREATE FUNCTION progress_gate(i integer, k integer) RETURNS integer
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
  PERFORM pg_advisory_xact_lock(7002, k);
  RETURN i;
END
$$;
]);

my $blocker = $node->background_psql('postgres');
my $copy = $node->background_psql('postgres');
my $index = $node->background_psql('postgres', on_error_stop => 0);
my $query = $node->background_psql('postgres');
my $copy_pid = $copy->query_safe('SELECT pg_backend_pid()');
my $index_pid = $index->query_safe('SELECT pg_backend_pid()');
my $query_pid = $query->query_safe('SELECT pg_backend_pid()');

sub wait_for_gate
{
	my ($pid, $key) = @_;
	$node->poll_query_until(
		'postgres', qq[
SELECT count(*) = 1 FROM pg_locks
WHERE pid = $pid AND locktype = 'advisory' AND NOT granted
  AND classid = 7002 AND objid = $key;
]) or die "backend $pid did not reach gate $key";
}

sub no_progress
{
	my ($pid, $description) = @_;
	is( $node->safe_psql(
			'postgres', qq[
SELECT count(*) FROM pg_stat_progress WHERE pid = $pid;
]),
		'0',
		$description);
}

$blocker->query_safe(
	q[
SELECT pg_advisory_lock(7002, 1), pg_advisory_lock(7002, 2),
       pg_advisory_lock(7002, 3);
]);
$copy->query_until(
	qr/copy_started/, q[
\echo copy_started
COPY (SELECT progress_gate(i, 1) FROM progress_data LIMIT 1) TO STDOUT;
]);
$index->query_until(
	qr/index_started/, q[
\echo index_started
CREATE INDEX progress_idx ON progress_data (progress_gate(i, 2));
]);
$query->query_until(
	qr/query_started/, q[
\echo query_started
SELECT progress_gate(i, 3) FROM progress_data LIMIT 1;
]);
wait_for_gate($copy_pid, 1);
wait_for_gate($index_pid, 2);
wait_for_gate($query_pid, 3);

is( $node->safe_psql(
		'postgres', qq[
SELECT command, progress_view FROM pg_stat_progress
WHERE pid IN ($copy_pid, $index_pid, $query_pid) ORDER BY command;
]),
	"COPY|pg_stat_progress_copy\nCREATE INDEX|pg_stat_progress_create_index",
	'overview lists simultaneous progress reporters and their detailed views'
);
is( $node->safe_psql(
		'postgres', qq[
SELECT (SELECT count(*) FROM pg_stat_progress_copy WHERE pid = $copy_pid),
       (SELECT count(*) FROM pg_stat_progress_create_index WHERE pid = $index_pid);
]),
	'1|1',
	'overview rows have corresponding detailed progress rows');
no_progress($query_pid,
	'an ordinary active query is not a progress reporter');

is( $node->safe_psql(
		'postgres', qq[
SET ROLE progress_viewer;
SELECT command, progress_view FROM pg_stat_progress
WHERE pid IN ($copy_pid, $index_pid) ORDER BY command;
]),
	"COPY|pg_stat_progress_copy\nCREATE INDEX|pg_stat_progress_create_index",
	'all users can discover progress reporters');
is( $node->safe_psql(
		'postgres', qq[
SET ROLE progress_viewer;
SELECT relid IS NULL FROM pg_stat_progress_copy WHERE pid = $copy_pid;
]),
	't',
	'discovering a reporter does not expose restricted details');

$blocker->query_safe('SELECT pg_advisory_unlock(7002, 1)');
$copy->query_safe('SELECT 1');
no_progress($copy_pid, 'completed COPY disappears from the overview');
is( $node->safe_psql(
		'postgres', qq[
SELECT command FROM pg_stat_progress WHERE pid = $index_pid;
]),
	'CREATE INDEX',
	'another active reporter remains visible');
is($node->safe_psql('postgres', "SELECT pg_cancel_backend($index_pid)"),
	't', 'cancel the index build');
$index->query('SELECT 1');
like(
	$index->{stderr},
	qr/canceling statement due to user request/,
	'the index build was canceled');
no_progress($index_pid, 'canceled index build disappears from the overview');
$blocker->query_safe(
	'SELECT pg_advisory_unlock(7002, 2), pg_advisory_unlock(7002, 3)');
$query->query_safe('SELECT 1');
$index->quit;
$query->quit;

$index = $node->background_psql('postgres');
$index_pid = $index->query_safe('SELECT pg_backend_pid()');
$index->query_safe(
	'CREATE INDEX progress_idx ON progress_data (progress_gate(i, 2))');
$blocker->query_safe('SELECT pg_advisory_lock(7002, 2)');
$index->query_until(
	qr/cluster_started/, q[
\echo cluster_started
CLUSTER progress_data USING progress_idx;
]);
wait_for_gate($index_pid, 2);
is( $node->safe_psql(
		'postgres', qq[
SELECT command, progress_view FROM pg_stat_progress WHERE pid = $index_pid;
]),
	'REPACK|pg_stat_progress_repack',
	'CLUSTER has one canonical overview row');
is( $node->safe_psql(
		'postgres', qq[
SELECT count(*) FROM pg_stat_progress_cluster WHERE pid = $index_pid;
]),
	'1',
	'the compatibility view remains available for the same backend');
$blocker->query_safe('SELECT pg_advisory_unlock(7002, 2)');
$index->query_safe('SELECT 1');
no_progress($index_pid, 'completed CLUSTER disappears from the overview');

$copy->quit;
$index->quit;
$blocker->quit;
$node->stop;
done_testing();
