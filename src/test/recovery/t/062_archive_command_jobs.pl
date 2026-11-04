# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('jobs');
$node->init(has_archiving => 1, allows_streaming => 1);

if ($PostgreSQL::Test::Utils::windows_os)
{
	$node->start;
	my $offset = -s $node->logfile;
	$node->append_conf('postgresql.conf', 'archive_command_jobs = 2');
	$node->reload;
	$node->wait_for_log(
		qr/"archive_command_jobs" greater than 1 is not supported on Windows/,
		$offset);
	is( $node->safe_psql('postgres', 'SHOW archive_command_jobs'),
		'1', 'archive_command_jobs > 1 rejected on Windows');
	$node->stop;
	done_testing();
	exit;
}

$node->append_conf('postgresql.conf', 'archive_command_jobs = 2');
$node->start;
$node->safe_psql('postgres', 'CREATE TABLE t_arch(i int)');

sub switch_wal
{
	my ($tag) = @_;
	$node->safe_psql('postgres',
		"INSERT INTO t_arch VALUES ($tag); SELECT pg_switch_wal()");
}

sub archived_count
{
	return $node->safe_psql('postgres',
		q{SELECT archived_count FROM pg_stat_archiver});
}

sub wait_archived_gt
{
	my ($before, $msg) = @_;
	ok(
		$node->poll_query_until(
			'postgres',
			qq{SELECT archived_count > $before FROM pg_stat_archiver},
			't'),
		$msg);
}

sub reload_jobs
{
	my ($jobs, $extra) = @_;
	my $offset = -s $node->logfile;
	$node->append_conf(
		'postgresql.conf',
		qq{archive_command_jobs = $jobs\n} . ($extra // ''));
	$node->reload;
	$node->wait_for_log(
		qr/restarting archiver process because value of "archive_command_jobs" was changed/,
		$offset);
	$node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'archiver'},
		't')
	  or die 'timed out waiting for archiver after archive_command_jobs change';
}

sub wait_for_hang_pids
{
	my ($hang_log, $want) = @_;
	my @pids;

	for (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		if (-f $hang_log)
		{
			@pids = grep { length } split /\n/, slurp_file($hang_log);
			last if @pids >= $want;
		}
		usleep(100_000);
	}
	return @pids;
}

# Basic async archive.
for (1 .. 3) { switch_wal(1); }
ok(
	$node->poll_query_until(
		'postgres', q{SELECT archived_count >= 3 FROM pg_stat_archiver}, 't'),
	'archive_command_jobs=2 archives WAL');

# Reload between async and sync modes.
reload_jobs(3);
reload_jobs(1);
is( $node->safe_psql('postgres', 'SHOW archive_command_jobs'),
	'1', 'SHOW archive_command_jobs is 1 after reload');
my $before = archived_count();
switch_wal(10);
wait_archived_gt($before, 'archive advances after reload to jobs=1');

# Failure under jobs=2, then recover.
reload_jobs(2);
my $bad_cmd = 'cp "%p_does_not_exist" "%f_does_not_exist"';
my $failed_before = $node->safe_psql('postgres',
	q{SELECT failed_count FROM pg_stat_archiver});

$node->safe_psql(
	'postgres', qq{
    ALTER SYSTEM SET archive_command TO '$bad_cmd';
    SELECT pg_reload_conf();
});
switch_wal(11);
ok(
	$node->poll_query_until(
		'postgres',
		qq{SELECT failed_count > $failed_before FROM pg_stat_archiver},
		't'),
	'failing archive_command under jobs=2 increments failed_count');

# Use the segment the archiver actually failed on, not the pre-switch LSN.
my $segment_name = $node->safe_psql('postgres',
	q{SELECT last_failed_wal FROM pg_stat_archiver});
ok($segment_name ne '', 'last_failed_wal reported after failure');
my $segment_ready =
  $node->data_dir . "/pg_wal/archive_status/$segment_name.ready";
ok(-f $segment_ready, ".ready retained for $segment_name after failure");

$node->safe_psql(
	'postgres', q{
	ALTER SYSTEM RESET archive_command;
	SELECT pg_reload_conf();
});
$before = archived_count();
switch_wal(12);
wait_archived_gt($before, 'archival recovers after good archive_command');
{
	my $cleared = 0;
	for (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		if (!-f $segment_ready)
		{
			$cleared = 1;
			last;
		}
		usleep(100_000);
	}
	ok($cleared, ".ready cleared for $segment_name after recovery");
}

# Concurrent hang jobs and shutdown cancel.
my $hang = $node->basedir . '/hang_archive.sh';
my $hang_log = $node->basedir . '/hang_archive.log';
my $good_cmd = 'cp "%p" "' . $node->archive_dir . '/%f"';
append_to_file(
	$hang, qq{#!/bin/sh
echo \$\$ >>'$hang_log'
sleep 1000
});
chmod 0755, $hang or die "chmod $hang: $!";

unlink $hang_log if -f $hang_log;
reload_jobs(3, qq{archive_command = '$hang "%p" "%f"'\n});
switch_wal(2);
switch_wal(3);
my @hang_pids = wait_for_hang_pids($hang_log, 2);
cmp_ok(scalar(@hang_pids), '>=', 2, 'two archive_command jobs ran concurrently');

reload_jobs(2, qq{archive_command = '$good_cmd'\n});
ok(!kill(0, $_), "hang archive pid $_ gone after reload") for @hang_pids;
$before = archived_count();
switch_wal(13);
wait_archived_gt($before, 'archiving drains after restoring good archive_command');

unlink $hang_log if -f $hang_log;
reload_jobs(3, qq{archive_command = '$hang "%p" "%f"'\n});
switch_wal(4);
switch_wal(5);
@hang_pids = wait_for_hang_pids($hang_log, 2);
cmp_ok(scalar(@hang_pids), '>=', 2, 'two hang archive_command jobs before stop');

my $offset = -s $node->logfile;
# Allow headroom past PGARCH_ASYNC_STOP_GRACE_S.
ok($node->stop('fast', timeout => 120),
	'stop with hung archive_command jobs');
like(
	slurp_file($node->logfile, $offset),
	qr/canceling in flight archive commands due to shutdown/,
	'shutdown canceled in flight archive commands after hung job grace');
ok(!kill(0, $_), "hang archive pid $_ gone after stop") for @hang_pids;

done_testing();
