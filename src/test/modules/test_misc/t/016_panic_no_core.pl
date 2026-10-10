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
io_method = worker
});
$node->start;

is($node->safe_psql('postgres', 'SHOW force_core_dump_on_panic'),
	'off', 'core dump override defaults to off');

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

sub check_panic
{
	my ($name, $action, $critical, $exit_code, $message) = @_;

	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$name', '$action')");
	my $log_offset = -s $node->logfile;
	my $run_function =
	  $critical
	  ? 'injection_points_run_in_critical_section'
	  : 'injection_points_run';
	my ($ret) = $node->psql(
		'postgres', qq{
SET log_min_messages = panic;
SELECT injection_points_intercept_abort();
SELECT $run_function('$name');
});

	isnt($ret, 0, "$name terminates the backend");
	$node->wait_for_log(qr/PANIC:.*\Q$message\E/, $log_offset);
	$node->wait_for_log(qr/\(PID \d+\) exited with exit code $exit_code/,
		$log_offset);
	ok($node->poll_query_until('postgres', undef, ''),
		"server recovers from $name");
}

check_panic('panic-no-core-promoted', 'error_disk_full', 1, 2,
	'error triggered for injection point panic-no-core-promoted');
check_panic('panic-no-core-explicit', 'panic_disk_full', 0, 2,
	'panic triggered for injection point panic-no-core-explicit');
check_panic('panic-no-core-rethrow', 'error_disk_full_rethrow_panic',
	0, 2,
	'rethrow disk-full PANIC for injection point panic-no-core-rethrow');
check_panic('panic-core-io', 'error_io', 1, 42,
	'I/O error triggered for injection point panic-core-io');
check_panic('panic-core-nested', 'panic_with_nested_disk_full', 0, 42,
	'nested disk-full PANIC');
check_panic('panic-core-promoted', 'error', 1, 42,
	'error triggered for injection point panic-core-promoted');
check_panic('panic-core-reenabled', 'panic_core_reenabled', 0, 42,
	'core dump re-enabled for injection point panic-core-reenabled');

$node->safe_psql('postgres',
	'ALTER SYSTEM SET force_core_dump_on_panic = on');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
ok( $node->poll_query_until(
		'postgres',
		q{SELECT current_setting('force_core_dump_on_panic') = 'on'}),
	'core dump override is active');

check_panic('panic-core-override', 'panic_disk_full', 0, 42,
	'panic triggered for injection point panic-core-override');

$node->stop;
done_testing();
