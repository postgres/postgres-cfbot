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
});
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE growing AS SELECT 1 AS i;
});

# Start without a strategy, grow the relation, then rewind the same scan.  The
# rescan must recreate its read stream with a bulk-read strategy.
my $bulkread = $node->safe_psql(
	'postgres', q{
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT FROM growing;
MOVE FORWARD ALL IN c;

INSERT INTO growing SELECT generate_series(2, 10000);

MOVE ABSOLUTE 0 IN c;
MOVE FORWARD ALL IN c;
COMMIT;

SELECT pg_stat_force_next_flush()
\gset

SELECT EXISTS (
  SELECT
  FROM pg_stat_get_backend_io(pg_backend_pid())
  WHERE object = 'relation'
    AND context = 'bulkread'
    AND reads + hits > 0
);
});

is($bulkread, 't',
	'a rescanned sequential scan uses its newly selected bulk-read strategy');

$node->stop;
done_testing();
