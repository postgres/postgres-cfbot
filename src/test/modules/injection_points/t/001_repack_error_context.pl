# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that an error propagated by the REPACK decoding worker does not use
# a current context on the backend.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('repack_error_context');
# wal_level must be higher than 'replica'
$node->init(allows_streaming => 1);
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# Setup a node
$node->safe_psql('postgres',
	"CREATE EXTENSION injection_points WITH SCHEMA public;");
$node->safe_psql('postgres',
	"CREATE TABLE repack_error_context_test (i integer PRIMARY KEY);");
$node->safe_psql('postgres',
	"INSERT INTO repack_error_context_test SELECT generate_series(1, 100);");

# Define a function that triggers an injection point. The backend would wait at
# this point by the upcoming test.
$node->safe_psql('postgres', qq[
	CREATE FUNCTION repack_error_context(integer) RETURNS integer
	LANGUAGE plpgsql IMMUTABLE AS \$\$
	BEGIN
		PERFORM public.injection_points_run('repack-leader-error-context');
		RETURN \$1;
	END;
	\$\$;
]);

# Define an index which uses the repack_error_context function
$node->safe_psql('postgres',
	"CREATE INDEX repack_error_context_idx ON repack_error_context_test (repack_error_context(i));");

$node->safe_psql('postgres',
	"SELECT injection_points_attach('repack-leader-error-context', 'wait');");

my $log_offset = -s $node->logfile;

# Run REPACK CONCURRENTLY. The command would wait at the injection point.
my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_until(
	qr/repack_started/,
	q[
\echo repack_started
REPACK (CONCURRENTLY) repack_error_context_test;
]);
$node->wait_for_event('client backend', 'repack-leader-error-context');

# Attach another injection point for the repack worker to raise an error
$node->safe_psql('postgres',
	"SELECT injection_points_attach('repack-worker-error-context', 'error')");

$node->wait_for_log(
	qr/client backend.*ERROR:  error triggered for injection point repack-worker-error-context/,
	$log_offset);

$session->quit;

my $log_contents = slurp_file($node->logfile, $log_offset);
unlike(
	$log_contents,
	qr/PL\/pgSQL function public\.repack_error_context/,
	'worker error does not use the backend PL/pgSQL context');

done_testing();
