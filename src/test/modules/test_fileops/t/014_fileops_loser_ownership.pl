# Copyright (c) 2026, PostgreSQL Global Development Group

# An online ATM entry does not own FILEOPS if the server crashes before
# writing the abort record.  A failed startup inverse must remain retryable.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';
plan skip_all => 'POSIX permission semantics required' if $windows_os;

my $node = PostgreSQL::Test::Cluster->new('loser_ownership');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
undo_instant_abort_threshold = 0
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_fileops;
CHECKPOINT;
SELECT injection_points_attach('undo-apply-before-batch', 'error');
SELECT injection_points_attach('undo-xact-abort-after-atm', 'wait');
});
my $dir = $node->data_dir . '/blocked';
mkdir($dir) or die "mkdir $dir: $!";
my $path = "$dir/loser.dat";
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(
	qr/start/, qq(\\echo start
BEGIN;
SELECT test_fileops_create('blocked/loser.dat', 384);
ROLLBACK;
));
$node->wait_for_event('client backend', 'undo-xact-abort-after-atm');
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
	't',
	'online abort registered in ATM before abort WAL');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$bg->{run}->finish;
chmod(0500, $dir) or die "chmod $dir: $!";
$node->start;
ok(-e $path, 'failed startup inverse leaves loser file');
like(
	slurp_file($node->logfile),
	qr/FILEOPS UNDO CREATE: could not unlink/,
	'startup attempted the inverse');
chmod(0700, $dir) or die "chmod $dir: $!";
# Persist the replacement marker before allowing the worker to run.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	'worker drained ATM');
ok(!-e $path, 'worker retries FILEOPS without a durable abort record');
$node->stop;
done_testing();
