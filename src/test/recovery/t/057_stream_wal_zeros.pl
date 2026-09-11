# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub files_are_equal
{
	my ($left, $right) = @_;
	open(my $left_fh, '<:raw', $left) or die "could not open $left: $!";
	open(my $right_fh, '<:raw', $right) or die "could not open $right: $!";

	while (1)
	{
		my ($left_buf, $right_buf);
		my $left_len = read($left_fh, $left_buf, 64 * 1024);
		my $right_len = read($right_fh, $right_buf, 64 * 1024);
		die "could not read WAL files: $!"
		  if !defined($left_len) || !defined($right_len);
		return 0 if $left_len != $right_len || $left_buf ne $right_buf;
		last if $left_len == 0;
	}

	close($left_fh) or die "could not close $left: $!";
	close($right_fh) or die "could not close $right: $!";
	return 1;
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# The walreceiver connects some time after the standby starts accepting
# connections, so wait for its START_REPLICATION to show up rather than
# reading pg_stat_activity once.
ok( $primary->poll_query_until(
		'postgres',
		q[SELECT EXISTS (SELECT 1 FROM pg_stat_activity
		  WHERE backend_type = 'walsender'
		  AND query LIKE '%(SKIP_WAL_PADDING)')]),
	'standby requests compact WAL padding messages');

# Start near the beginning of a segment, then generate a small amount of WAL
# so that the next switch leaves a large zero-filled tail.
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_replay_catchup($standby);

# Count only the walreceiver's writes for the segment under test.  Counters
# still pending in the walreceiver from before the reset can only make the
# total larger.
$standby->safe_psql('postgres', "SELECT pg_stat_reset_shared('io')");
$primary->safe_psql('postgres',
	'CREATE TABLE stream_wal_zeros AS SELECT generate_series(1, 10) AS i');

my $walfile = $primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_switch_wal())');
my $flush_lsn = $primary->lsn('flush');
$primary->wait_for_catchup($standby, 'flush', $flush_lsn);

my $primary_path = $primary->data_dir . "/pg_wal/$walfile";
my $standby_path = $standby->data_dir . "/pg_wal/$walfile";

ok(files_are_equal($standby_path, $primary_path),
	'streamed WAL segment is reconstructed byte for byte');

# The bytes match whether the padding was streamed or skipped, so also check
# that the walreceiver did not write a whole segment.  Restart the standby
# to flush its I/O statistics first.
$standby->restart;

my $written = $standby->safe_psql('postgres',
	q[SELECT write_bytes FROM pg_stat_io
	  WHERE backend_type = 'walreceiver' AND object = 'wal'
	  AND context = 'normal']);
my $segsize = $standby->safe_psql('postgres',
	q[SELECT setting FROM pg_settings WHERE name = 'wal_segment_size']);
cmp_ok($written, '>', 0, 'walreceiver reported its WAL writes');
cmp_ok($written, '<', $segsize,
	'walreceiver did not write the zero-filled padding');

done_testing();
