# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the bounded binary search for WAL time boundaries across segments.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('time_search');
$node->init(extra => ['--wal-segsize=1']);
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION pg_walinspect; CREATE TABLE test_table (a int);');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

my ($lower_time, $upper_time);
for my $i (0 .. 8)
{
	$node->safe_psql('postgres', "INSERT INTO test_table VALUES ($i)");
	$lower_time = $node->safe_psql('postgres', 'SELECT clock_timestamp()')
	  if $i == 3;
	$upper_time = $node->safe_psql('postgres', 'SELECT clock_timestamp()')
	  if $i == 5;
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()') if $i < 8;
}

my $log_offset = -s $node->logfile;
my $result = $node->safe_psql(
	'postgres', qq{
SET log_min_messages = debug1;
SELECT start_timestamp >= '$lower_time'::timestamptz - interval '1 microsecond',
       end_timestamp > '$lower_time'::timestamptz,
       end_timestamp <= '$upper_time'::timestamptz,
       start_lsn < end_lsn,
       pg_walfile_name(start_lsn) <> pg_walfile_name(end_lsn)
FROM pg_get_wal_location_at_time('$lower_time',
                                 interval '1 microsecond',
                                 '$upper_time'::timestamptz -
                                   '$lower_time'::timestamptz)});
is($result, 't|t|t|t|t', 'binary search finds boundaries across WAL segments');

my $log = slurp_file($node->logfile, $log_offset);
$log =~ /WAL time search decoded (\d+) of (\d+) retained segments/
  or die "could not find WAL time search statistics in server log";
cmp_ok($1, '<', $2, 'binary search does not decode all retained WAL');

$node->stop;
done_testing();
