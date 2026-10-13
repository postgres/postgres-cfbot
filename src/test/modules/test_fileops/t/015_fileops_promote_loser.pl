# Copyright (c) 2026, PostgreSQL Global Development Group

# Promotion must not leave a FILEOPS loser eligible for another startup walk
# after a committed transaction has reused its path.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(time);
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
checkpoint_timeout = '1h'
checkpoint_completion_target = 0.9
max_wal_size = '2GB'
autovacuum = off
max_logical_revert_workers = 0
log_checkpoints = on
});
$primary->start;
$primary->safe_psql(
	'postgres', q{
CREATE EXTENSION test_fileops;
CREATE TABLE dirty (data text);
});
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# Dirty enough buffers that the asynchronous, time-smoothed checkpoint
# requested after fast promotion cannot finish before the immediate stop.
$primary->safe_psql('postgres',
	"INSERT INTO dirty SELECT repeat('x', 1000) FROM generate_series(1, 10000)"
);
my $bg = $primary->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_create('loser.dat', 384)");
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_catchup($standby);
my $path = $standby->data_dir . '/loser.dat';
ok(-e $path, 'standby replayed the loser create');
$primary->stop('immediate');
$bg->{run}->finish;

my $started = time();
$standby->promote;
diag(
	sprintf(
		'promotion with 10MB dirty data: %.3f seconds', time() - $started));
ok(!-e $path, 'promotion removed loser file');
my $checkpoint = $standby->safe_psql('postgres',
	'SELECT checkpoint_lsn FROM pg_control_checkpoint()');
$standby->safe_psql(
	'postgres', q{
BEGIN;
SELECT test_fileops_create('loser.dat', 384);
SELECT test_fileops_write('loser.dat', 0, 'committed-after-promotion');
COMMIT;
});
is( $standby->safe_psql(
		'postgres', 'SELECT checkpoint_lsn FROM pg_control_checkpoint()'),
	$checkpoint,
	'no checkpoint completed after promotion');

for my $restart (1, 2)
{
	$standby->stop('immediate');
	$standby->start;
	is( -e $path ? slurp_file($path) : '<missing>',
		'committed-after-promotion',
		"restart $restart preserves committed reuse");
}
$standby->stop;
done_testing();
