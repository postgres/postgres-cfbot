# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that a statistics snapshot excludes backends that started after the
# snapshot was created.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('stats_snapshot');
$node->init;
$node->start;

my $snapshot = $node->background_psql('postgres');

$snapshot->query_safe(
	q[
	BEGIN;
	SET LOCAL stats_fetch_consistency = 'snapshot';
	SELECT count(*) FROM pg_stat_wal;
]);

# Establish this connection only after the first session built its snapshot.
my $late = $node->background_psql('postgres');
my $late_pid = $late->query_safe('SELECT pg_backend_pid()');

# WAL returns one scalar composite, so count a field to distinguish a NULL
# result from the fabricated all-zero row.
is( $snapshot->query_safe(
		qq[
		SELECT
			(SELECT count(wal_records)
			 FROM pg_stat_get_backend_wal($late_pid)),
			(SELECT count(*) FROM pg_stat_get_backend_io($late_pid)),
			(SELECT count(*) FROM pg_stat_get_backend_lock($late_pid));
	]),
	'0|0|0',
	'statistics snapshot excludes a backend created later');

$snapshot->query_safe('ROLLBACK');
$late->quit;
$snapshot->quit;
$node->stop;

done_testing();
