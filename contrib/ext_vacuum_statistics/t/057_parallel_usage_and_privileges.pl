# Copyright (c) 2026, PostgreSQL Global Development Group

# Compare extended statistics with VACUUM's independent instrumentation,
# and check that resetting statistics requires an explicit privilege.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('usage');
$node->init;
$node->append_conf('postgresql.conf', q{
shared_preload_libraries = 'ext_vacuum_statistics'
autovacuum = off
max_parallel_maintenance_workers = 1
min_parallel_index_scan_size = 0
track_io_timing = on
});
$node->start;
$node->safe_psql('postgres', q{
CREATE EXTENSION ext_vacuum_statistics;
CREATE ROLE stats_reader;
GRANT USAGE ON SCHEMA ext_vacuum_statistics TO stats_reader;
CREATE TABLE usage_test (id integer, other integer) WITH (autovacuum_enabled = off);
CREATE INDEX usage_test_id ON usage_test (id);
CREATE INDEX usage_test_other ON usage_test (other);
});

# Include the parallel path with no workers available: the leader's index
# reports must also be subtracted from the table's resource usage.
for my $mode ('serial', 'parallel', 'no_workers', 'parallel_cleanup')
{
	my $delete = $mode eq 'parallel_cleanup' ? '' :
	  'DELETE FROM usage_test WHERE id % 2 = 0;';
	my $deleted = $mode eq 'parallel_cleanup' ? 0 : 25000;
	$node->safe_psql('postgres', qq{
TRUNCATE usage_test;
INSERT INTO usage_test SELECT i, -i FROM generate_series(1, 50000) i;
$delete
SELECT ext_vacuum_statistics.vacuum_statistics_reset();
CHECKPOINT;
});
	# Start with cold shared buffers so reads as well as hits are exercised.
	$node->restart;
	my $parallel = $mode eq 'serial' ? 0 : 1;
	my $workers = $mode eq 'no_workers' ? 0 : 1;
	my ($stdout, $stderr) = ('', '');
	is($node->psql('postgres', qq{
SET max_parallel_workers = $workers;
VACUUM (VERBOSE, PARALLEL $parallel, INDEX_CLEANUP ON, TRUNCATE OFF) usage_test;
}, stdout => \$stdout, stderr => \$stderr), 0, "$mode vacuum succeeds");
	if ($mode =~ /^parallel/)
	{
		like($stderr, qr/launched 1 parallel vacuum worker/, "$mode worker actually launched");
	}
	elsif ($mode eq 'no_workers')
	{
		like($stderr, qr/launched 0 parallel vacuum workers/, 'leader handles parallel path without workers');
	}

	my @buffers = $stderr =~ /buffer usage: (\d+) hits, (\d+) reads, (\d+) dirtied/;
	my @wal = $stderr =~ /WAL usage: (\d+) records, (\d+) full page images, (\d+) bytes/;
	is(scalar @buffers, 3, "$mode buffer instrumentation available");
	is(scalar @wal, 3, "$mode WAL instrumentation available");
	cmp_ok($buffers[1], '>', 0, "$mode performed buffer reads");
	my @expected = (@buffers, @wal);
	my @columns = qw(total_blks_hit total_blks_read total_blks_dirtied wal_records wal_fpi wal_bytes);
	my @db_columns = qw(db_blks_hit db_blks_read db_blks_dirtied db_wal_records db_wal_fpi db_wal_bytes);
	for my $i (0 .. $#columns)
	{
		my $column = $columns[$i];
		my $db_column = $db_columns[$i];
		my $sum = $node->safe_psql('postgres', qq{
SELECT sum($column) FROM (
 SELECT $column FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'usage_test'
 UNION ALL
 SELECT $column FROM ext_vacuum_statistics.pg_stats_vacuum_indexes WHERE indexrelname IN ('usage_test_id', 'usage_test_other')
) s;
});
		is($sum, $expected[$i], "$mode $column counted once across table and indexes");
		is($node->safe_psql('postgres', qq{
SELECT $db_column FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dbname = current_database();
}), $expected[$i], "$mode $column counted once in database");
	}
	is($node->safe_psql('postgres', qq{
SELECT count(*) FROM ext_vacuum_statistics.pg_stats_vacuum_indexes
WHERE indexrelname IN ('usage_test_id', 'usage_test_other') AND tuples_deleted = $deleted;
}), '2', "$mode reports both indexes");
}

my $dboid = $node->safe_psql('postgres', 'SELECT oid FROM pg_database WHERE datname = current_database()');
my $relid = $node->safe_psql('postgres', "SELECT 'usage_test'::regclass::oid");
my @resets = (
	['vacuum_statistics_reset()', 'vacuum_statistics_reset()'],
	['extvac_reset_entry(oid, oid)', "extvac_reset_entry($dboid, $relid)"],
	['extvac_reset_db_entry(oid)', "extvac_reset_db_entry($dboid)"]);
for my $reset (@resets)
{
	my ($signature, $call) = @$reset;
	my ($stdout, $stderr) = ('', '');
	is($node->psql('postgres', qq{
SET ROLE stats_reader;
SELECT ext_vacuum_statistics.$call;
}, stdout => \$stdout, stderr => \$stderr), 3, "$signature denied to ordinary role");
	like($stderr, qr/permission denied for function/, "$signature denied by function ACL");
	$node->safe_psql('postgres', qq{
GRANT EXECUTE ON FUNCTION ext_vacuum_statistics.$signature TO stats_reader;
SET ROLE stats_reader;
SELECT ext_vacuum_statistics.$call;
});
	pass("$signature succeeds after explicit grant");
}

$node->stop;
done_testing();
