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
$primary->append_conf('postgresql.conf', 'wal_init_zero = off');
$primary->start;

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'wal_init_zero = off');
$standby->start;

# Start near the beginning of a segment, then generate a small amount of WAL
# so that the next switch leaves a large zero-filled tail.
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_replay_catchup($standby);
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

SKIP:
{
	skip 'allocated block count is not portable to Windows', 1
	  if $^O eq 'MSWin32';

	my @st = stat($standby_path);
	skip 'filesystem does not report allocated blocks', 1
	  if !defined($st[12]) || $st[12] == 0;

	cmp_ok($st[12] * 512, '<', $st[7],
		'zero-filled WAL tail is stored sparsely on the standby');
}

done_testing();
