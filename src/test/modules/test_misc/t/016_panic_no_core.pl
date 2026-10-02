# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('panic_no_core');
$node->init;
# Avoid exhausting io_uring resources across repeated crash recovery.
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
log_min_messages = warning
force_core_dump_on_panic = off
io_method = worker
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

sub check_panic
{
	my ($arguments, $exit_code, $description) = @_;

	my $log_offset = -s $node->logfile;
	$node->psql(
		'postgres', "SELECT injection_points_test_panic_core_dump($arguments)");

	$node->wait_for_log(qr/PANIC:.*PANIC requested by test/, $log_offset);
	$node->wait_for_log(qr/\(PID \d+\) exited with exit code $exit_code/,
		$log_offset);
	ok($node->poll_query_until('postgres', undef, ''), $description);
}

check_panic('true', 2, 'disk-full PANIC exits without abort');
check_panic('false', 42, 'other PANIC errors still abort');
check_panic('true, true', 42, 'errabort(true) restores abort for disk-full PANIC');

$node->safe_psql('postgres',
	'ALTER SYSTEM SET force_core_dump_on_panic = on');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
ok( $node->poll_query_until(
		'postgres',
		q{SELECT current_setting('force_core_dump_on_panic') = 'on'}),
	'core dump override is active');

check_panic('true', 42, 'override forces abort for disk-full PANIC');

$node->stop;
done_testing();
