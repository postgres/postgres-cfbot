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
SELECT start_timestamp <= '$lower_time'::timestamptz - interval '1 microsecond',
       end_timestamp >= '$upper_time'::timestamptz,
       start_lsn < end_lsn,
       pg_walfile_name(start_lsn) <> pg_walfile_name(end_lsn)
FROM pg_get_wal_location_at_time('$lower_time',
                                 interval '1 microsecond',
                                 '$upper_time'::timestamptz -
                                   '$lower_time'::timestamptz)});
is($result, 't|t|t|t', 'binary search finds outer boundaries across WAL segments');

my $log = slurp_file($node->logfile, $log_offset);
$log =~ /WAL time search decoded (\d+) of (\d+) retained segments/
  or die "could not find WAL time search statistics in server log";
cmp_ok($1, '<', $2, 'binary search does not decode all retained WAL');

# A window containing no timestamped record is enclosed by the timestamped
# records immediately outside it.
$result = $node->safe_psql(
	'postgres', q{
SELECT clock_timestamp() AS empty_window_start \gset
SELECT count(*) AS ignored FROM generate_series(1, 100000) \gset
SELECT clock_timestamp() AS empty_window_end \gset
INSERT INTO test_table VALUES (9);

SELECT location.start_timestamp < :'empty_window_start'::timestamptz,
       location.end_timestamp > :'empty_window_end'::timestamptz,
       location.start_lsn < location.end_lsn
FROM pg_get_wal_location_at_time(
       :'empty_window_start',
       interval '1 microsecond',
       :'empty_window_end'::timestamptz -
         :'empty_window_start'::timestamptz) AS location;});
is($result, 't|t|t',
	'outer anchors enclose a window with no timestamped record');

# With no later timestamped record, use the current WAL position as the upper
# boundary.  The returned range must include WAL generated before the only
# commit record in the requested window.
$result = $node->safe_psql(
	'postgres', q{
SELECT clock_timestamp() AS window_start \gset
BEGIN;
SELECT pg_current_wal_insert_lsn() AS delete_lsn \gset
DELETE FROM test_table WHERE a = 5;
COMMIT;
SELECT clock_timestamp() AS window_end \gset

SELECT location.start_timestamp <=
         :'window_start'::timestamptz - interval '1 microsecond',
       location.end_timestamp IS NULL,
       location.start_lsn < :'delete_lsn'::pg_lsn,
       location.end_lsn > :'delete_lsn'::pg_lsn,
       EXISTS (SELECT
               FROM pg_get_wal_records_info(location.start_lsn,
                                            location.end_lsn)
               WHERE resource_manager = 'Heap' AND record_type = 'DELETE')
FROM pg_get_wal_location_at_time(
       :'window_start',
       interval '1 microsecond',
       :'window_end'::timestamptz - :'window_start'::timestamptz) AS location;});
is($result, 't|t|t|t|t',
	'current WAL fallback includes changes preceding the only commit in the window');

$node->stop;
done_testing();
