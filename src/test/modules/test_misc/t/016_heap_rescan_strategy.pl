# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set a low threshold for selecting a bulk-read strategy.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_buffers = '1MB'
max_parallel_workers_per_gather = 0
});
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE growing AS
  SELECT g, repeat('x', 1000) AS payload
  FROM generate_series(1, 16) AS g;
CREATE TABLE growth_done (done boolean PRIMARY KEY);

CREATE FUNCTION grow_table(should_grow boolean)
RETURNS integer
LANGUAGE plpgsql
VOLATILE
AS $$
DECLARE
  inserted boolean;
BEGIN
  IF should_grow THEN
    INSERT INTO growth_done VALUES (true)
      ON CONFLICT DO NOTHING
      RETURNING true INTO inserted;

    IF inserted THEN
      INSERT INTO growing
        SELECT g, repeat('y', 1000)
        FROM generate_series(17, 4000) AS g;
    END IF;
  END IF;

  RETURN 1;
END
$$;
});

# Grow the relation during the second scan.  The third rescan must recreate
# its read stream with the newly selected bulk-read strategy.
my $bulkread = $node->safe_psql(
	'postgres', q{
SET enable_material = off;
SET enable_memoize = off;
SELECT count(*) AS scan_count
FROM (VALUES (1, false), (2, true), (3, false)) AS v(ord, should_grow)
CROSS JOIN LATERAL (
  SELECT *
  FROM growing
  WHERE g >= grow_table(v.should_grow)
  OFFSET 0
) AS s
\gset

SELECT pg_stat_force_next_flush()
\gset

SELECT COALESCE(sum(reads), 0) + COALESCE(sum(hits), 0)
FROM pg_stat_get_backend_io(pg_backend_pid())
WHERE object = 'relation'
  AND context = 'bulkread';
});

cmp_ok($bulkread, '>', 0,
	'a rescanned sequential scan uses its newly selected bulk-read strategy');

$node->stop;
done_testing();
