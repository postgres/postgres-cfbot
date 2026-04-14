# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('plpgsql_composite_replan_race');
$node->init;
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql('postgres', q[
CREATE TYPE planinv_ct AS (a int, b int);
CREATE TABLE planinv_tbl (a int, b int);
INSERT INTO planinv_tbl VALUES (1, 2);
CREATE FUNCTION planinv_srf() RETURNS SETOF planinv_ct
  LANGUAGE sql STABLE SECURITY DEFINER AS $$
    SELECT a, b FROM planinv_tbl
  $$;
CREATE FUNCTION planinv_caller() RETURNS SETOF planinv_ct LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT r.* FROM planinv_srf() r;
END;
$$;
]);

# Warm up expression/plan caches first.
is($node->safe_psql('postgres', 'SELECT * FROM planinv_caller();'), '1|2',
	'warmup call returns initial row shape');

my $backend2 = $node->background_psql('postgres', on_error_stop => 0);
$backend2->query_safe(q[
SELECT injection_points_set_local();
SELECT injection_points_attach('plpgsql-return-query-before-exec', 'wait');
]);

$backend2->query_until(
	qr/race_started/, q[
\echo race_started
BEGIN;
SELECT * FROM planinv_caller();
\echo race_done
]);

$node->poll_query_until('postgres', q[
SELECT EXISTS (
  SELECT 1
  FROM pg_stat_activity
  WHERE wait_event_type = 'InjectionPoint'
    AND wait_event = 'plpgsql-return-query-before-exec'
);
]) or die 'backend2 did not reach injection point in time';

$node->safe_psql('postgres', q[
BEGIN;
ALTER TYPE planinv_ct ADD ATTRIBUTE c int;
CREATE OR REPLACE FUNCTION planinv_srf() RETURNS SETOF planinv_ct
  LANGUAGE sql STABLE SECURITY DEFINER AS $$
    SELECT a, b, 99 FROM planinv_tbl
  $$;
COMMIT;
]);

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('plpgsql-return-query-before-exec');");

my $out = $backend2->query_until(qr/race_done/, q[]);
like($out, qr/^1\|2\|99$/m,
	'concurrent ALTER TYPE + CREATE OR REPLACE does not break RETURN QUERY');
is($backend2->{stderr}, '',
	'no tuple shape mismatch reported by RETURN QUERY');

ok($backend2->quit);

$node->safe_psql('postgres', q[
DROP FUNCTION planinv_caller();
DROP FUNCTION planinv_srf();
DROP TABLE planinv_tbl;
DROP TYPE planinv_ct;
]);

done_testing();
