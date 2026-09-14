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
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
log_min_messages = warning
});
$node->start;

is($node->safe_psql('postgres', 'SHOW force_core_dump_on_panic'),
	'off', 'core dump override defaults to off');

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
SELECT injection_points_attach('panic-no-core-promoted', 'error_disk_full');
});

my $log_offset = -s $node->logfile;
my ($ret) = $node->psql(
	'postgres',
	q{
SET log_min_messages = panic;
SELECT injection_points_run_in_critical_section('panic-no-core-promoted');
});

isnt($ret, 0, 'promoted no-core PANIC terminates the backend');

$node->wait_for_log(
	qr/PANIC:.*error triggered for injection point panic-no-core-promoted/,
	$log_offset);

# Exit code 2 proves that abort() was not used.
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 2/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from promoted no-core PANIC');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-no-core-explicit', 'panic_disk_full')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{
SET log_min_messages = panic;
SELECT injection_points_run('panic-no-core-explicit');
});

isnt($ret, 0, 'explicit no-core PANIC terminates the backend');
$node->wait_for_log(
	qr/PANIC:.*panic triggered for injection point panic-no-core-explicit/,
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 2/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from explicit no-core PANIC');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-no-core-rethrow', 'error_disk_full_rethrow_panic')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{SELECT injection_points_run('panic-no-core-rethrow')});

isnt($ret, 0, 'rethrown no-core PANIC terminates the backend');
$node->wait_for_log(
	qr/PANIC:.*rethrow disk-full PANIC for injection point panic-no-core-rethrow/,
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 2/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from rethrown no-core PANIC');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-core-io', 'error_io')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{
SELECT injection_points_intercept_abort();
SELECT injection_points_run_in_critical_section('panic-core-io');
});

isnt($ret, 0, 'non-ENOSPC PANIC terminates the backend via abort');
$node->wait_for_log(
	qr{PANIC:.*I/O error triggered for injection point panic-core-io},
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 42/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from non-ENOSPC PANIC');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-core-nested', 'panic_with_nested_disk_full')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{
SELECT injection_points_intercept_abort();
SELECT injection_points_run('panic-core-nested');
});

isnt($ret, 0, 'outer core-worthy PANIC terminates the backend via abort');
$node->wait_for_log(qr/PANIC:.*nested disk-full PANIC/,
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 42/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'core-worthy outer PANIC wins over nested suppression');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-core-promoted', 'error')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{
SELECT injection_points_intercept_abort();
SELECT injection_points_run_in_critical_section('panic-core-promoted');
});

isnt($ret, 0, 'unannotated promoted PANIC terminates the backend');
$node->wait_for_log(
	qr/PANIC:.*error triggered for injection point panic-core-promoted/,
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 42/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from unannotated promoted PANIC');

$node->safe_psql('postgres',
	'ALTER SYSTEM SET force_core_dump_on_panic = on');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
ok($node->poll_query_until(
		'postgres',
		q{SELECT current_setting('force_core_dump_on_panic') = 'on'}),
	'core dump override is active');

$node->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('panic-core-override', 'panic_disk_full')});

$log_offset = -s $node->logfile;
($ret) = $node->psql(
	'postgres',
	q{
SELECT injection_points_intercept_abort();
SELECT injection_points_run('panic-core-override');
});

isnt($ret, 0, 'core dump override terminates the backend via abort');
$node->wait_for_log(
	qr/PANIC:.*panic triggered for injection point panic-core-override/,
	$log_offset);
$node->wait_for_log(qr/\(PID \d+\) exited with exit code 42/,
	$log_offset);

is($node->poll_query_until('postgres', undef, ''),
	'1', 'server recovers from overridden no-core PANIC');

$node->stop;
done_testing();
