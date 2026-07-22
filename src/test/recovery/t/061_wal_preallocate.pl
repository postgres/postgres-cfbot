# Copyright (c) 2026, PostgreSQL Global Development Group

# Test pg_wal_preallocate().
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Return regular WAL segment files (24 hex digits) in a node's pg_wal.
sub get_segments
{
	my $node = shift;
	my $waldir = $node->data_dir . '/pg_wal';
	opendir(my $dh, $waldir) or die "could not open $waldir: $!";
	my @segs = grep { /^[0-9A-F]{24}$/ } readdir($dh);
	closedir($dh);
	return sort @segs;
}

sub count_segments
{
	my @segments = get_segments(shift);
	return scalar @segments;
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
checkpoint_timeout = 1h
min_wal_size = 2MB
max_wal_size = 1GB
wal_recycle = off
});
$node->start;

my $segsize = $node->safe_psql('postgres',
	"SELECT pg_size_bytes(current_setting('wal_segment_size'))");

# Exercise the insertion position immediately after a segment switch.
$node->safe_psql('postgres', q{
CHECKPOINT;
SELECT pg_switch_wal();
SELECT pg_create_restore_point('preallocation boundary test');
});
my $current_segment = $node->safe_psql(
	'postgres', "SELECT pg_walfile_name(pg_current_wal_insert_lsn())");
my @segments = get_segments($node);
my $last_existing_segment = $segments[-1];

# Consume future files left by initdb without recycling replacements.
while ($current_segment lt $last_existing_segment)
{
	$node->safe_psql('postgres', q{
SELECT pg_switch_wal();
SELECT pg_create_restore_point('preallocation boundary test');
});
	$current_segment = $node->safe_psql(
		'postgres', "SELECT pg_walfile_name(pg_current_wal_insert_lsn())");
}

@segments = get_segments($node);

my $last_segment = $node->safe_psql(
	'postgres',
	"SELECT pg_walfile_name(pg_current_wal_insert_lsn() + (8 * $segsize)::bigint)");
my $last_path = $node->data_dir . "/pg_wal/$last_segment";

my $before = scalar @segments;
my $created =
  $node->safe_psql('postgres', "SELECT pg_wal_preallocate(8 * $segsize)");
is($created, 8, 'preallocation starts after the exact insertion segment');
is(count_segments($node), $before + $created,
	'pg_wal grew by the reported number of segments');
ok(-f $last_path, 'last requested future segment exists');
is($node->safe_psql('postgres', "SELECT pg_wal_preallocate(8 * $segsize)"),
	0, 'existing files are not recreated');
is($node->safe_psql('postgres', 'SELECT pg_wal_preallocate()'),
	0, 'omitted size uses min_wal_size');

my $limit_before = count_segments($node);
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', "SELECT pg_wal_preallocate(1025 * $segsize)");
isnt($ret, 0, 'request above max_wal_size is rejected');
like($stderr, qr/exceeds "max_wal_size"/, 'error reports max_wal_size');
is(count_segments($node), $limit_before, 'rejected request creates nothing');

$node->stop;

done_testing();
