# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# A crash during ROLLBACK PREPARED must not lose the rollback.  After the
# abort record nothing is left prepared, so the chain must already be in the
# ATM by then.  Before the abort record the transaction stays prepared, and
# nothing may be reverted until it is rolled back again; it may also be
# committed instead.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Every assertion here is a file mode; Windows has no POSIX file modes.
plan skip_all => 'POSIX permission semantics required' if $windows_os;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_rp_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'injection_points'
max_prepared_transactions = 5
wal_level = replica
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $file = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('rp.dat')");

sub crash_in_rollback_prepared
{
	my ($ip) = @_;
	$node->safe_psql('postgres', "SELECT test_fileops_chmod('$file', 420)");
	$node->safe_psql(
		'postgres', qq{
BEGIN;
SELECT test_fileops_chmod('$file', 448);
PREPARE TRANSACTION 'rp';
});
	is( $node->safe_psql('postgres', "SELECT test_fileops_get_mode('$file')"),
		'448',
		"$ip: the prepared chmod is on disk");
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$ip', 'wait')");
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_until(
		qr/start/, qq(\\echo start
ROLLBACK PREPARED 'rp';
));
	$node->wait_for_event('client backend', $ip);
	$node->stop('immediate');
	$bg->{run}->finish;
	$node->start;
}

crash_in_rollback_prepared('rollback-prepared-after-abort-record');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
	'0', 'after the abort record: no longer prepared');
ok( $node->poll_query_until(
		'postgres', "SELECT test_fileops_get_mode('$file') = 420"),
	'after the abort record: the chmod is reverted');

crash_in_rollback_prepared('rollback-prepared-before-abort-record');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
	'1', 'before the abort record: still prepared');
sleep(2);
is($node->safe_psql('postgres', "SELECT test_fileops_get_mode('$file')"),
	'448', 'before the abort record: nothing reverted while still prepared');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'rp'");
ok( $node->poll_query_until(
		'postgres', "SELECT test_fileops_get_mode('$file') = 420"),
	'before the abort record: ROLLBACK PREPARED again reverts the chmod');

# Or the transaction is committed after all, and keeps its change.
crash_in_rollback_prepared('rollback-prepared-before-abort-record');
$node->safe_psql('postgres', "COMMIT PREPARED 'rp'");
sleep(2);
is($node->safe_psql('postgres', "SELECT test_fileops_get_mode('$file')"),
	'448', 'before the abort record: COMMIT PREPARED keeps the chmod');
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	't',
	'and leaves no ATM entry behind');

$node->stop;
done_testing();
