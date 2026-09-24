# Copyright (c) 2026, PostgreSQL Global Development Group

# Test fast shutdown during crash restart, before WAL redo has started.

use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);

# Make the restarted startup process wait in restore_command until shutdown.
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $windows_os;
my $logfile = $node->logfile;
$logfile =~ s!\\!/!g if $windows_os;
my $timeout = $PostgreSQL::Test::Utils::timeout_default;
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
restore_command = '"$perlbin" "$FindBin::RealBin/wait_for_shutdown" "$logfile" $timeout'
});
$node->start;

$node->poll_query_until(
	'postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity
	  WHERE backend_type = 'background writer'}
) or die 'background writer did not start';
my $pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'background writer'"
);
$node->set_standby_mode;
system_or_bail('pg_ctl', 'kill', 'QUIT', $pid);
$node->wait_for_log(qr/restore_command waiting for shutdown/);

ok( $node->stop('fast', fail_ok => 1, timeout => $timeout),
	'fast shutdown completes during crash restart');
# If the helper gave up, startup failed and the shutdown above proves nothing.
unlike(
	slurp_file($node->logfile),
	qr/timed out waiting for shutdown request/,
	'restore_command did not time out');

done_testing();
