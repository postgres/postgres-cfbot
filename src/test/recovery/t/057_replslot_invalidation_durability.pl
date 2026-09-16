# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test that replication slot invalidation is persisted before it is published.
#
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$node->append_conf(
	'postgresql.conf', qq(
checkpoint_timeout = 1h
min_wal_size = 2MB
max_wal_size = 64MB
wal_keep_size = 0
max_slot_wal_keep_size = -1
log_checkpoints = on
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('target_slot', true)});
$node->safe_psql('postgres', 'CHECKPOINT');

my ($restart_lsn, $restart_segment) = split(
	/\|/,
	$node->safe_psql(
		'postgres',
		q{
SELECT restart_lsn, pg_walfile_name(restart_lsn)
FROM pg_replication_slots
WHERE slot_name = 'target_slot'
}));
my $restart_segment_path = $node->data_dir . "/pg_wal/$restart_segment";
my $inactive_since = $node->safe_psql(
	'postgres',
	q{
SELECT inactive_since
FROM pg_replication_slots
WHERE slot_name = 'target_slot'
});

$node->append_conf('postgresql.conf', 'max_slot_wal_keep_size = 1MB');
$node->reload;
$node->advance_wal(8);

my $current_segment = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
isnt($current_segment, $restart_segment,
	'target slot requires an older WAL segment');
ok(-f $restart_segment_path,
	"target slot WAL segment $restart_segment exists before invalidation");

$node->safe_psql(
	'postgres', q{
SELECT injection_points_attach(
	'replication-slot-save-error', 'error', 'target_slot')
});

my ($ret, $stdout, $stderr) = $node->psql('postgres', 'CHECKPOINT');
like(
	$stderr,
	qr/checkpoint request failed/,
	'injected slot save error failed the checkpoint');

$node->safe_psql('postgres',
	q{SELECT injection_points_detach('replication-slot-save-error')});

is( $node->safe_psql(
		'postgres',
		qq{
SELECT NOT active, invalidation_reason IS NULL,
       restart_lsn = '$restart_lsn',
       inactive_since = '$inactive_since'::timestamptz
FROM pg_replication_slots
WHERE slot_name = 'target_slot'
}),
	't|t|t|t',
	'failed save leaves the valid slot unchanged');
ok( -f $restart_segment_path,
	"target slot WAL segment $restart_segment survives the failed checkpoint"
);

$node->append_conf('postgresql.conf', 'max_slot_wal_keep_size = -1');
$node->reload;
$node->safe_psql('postgres', 'CHECKPOINT');

ok(-f $restart_segment_path,
	"target slot WAL segment $restart_segment survives the next checkpoint");

$node->stop('immediate');
$node->start;

is( $node->safe_psql(
		'postgres',
		qq{
SELECT NOT active, invalidation_reason IS NULL,
       restart_lsn = '$restart_lsn'
FROM pg_replication_slots
WHERE slot_name = 'target_slot'
}),
	't|t|t',
	'target slot restores with its original restart LSN');
ok(-f $restart_segment_path,
	"target slot WAL segment $restart_segment exists after restart");

$node->stop;

# Check that slot synchronization also persists an invalidation before
# publishing it.
my $primary = PostgreSQL::Test::Cluster->new('sync_primary');
$primary->init(allows_streaming => 'logical', extra => ['--wal-segsize=1']);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 64MB
));
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('sync_phys')});
$primary->backup('sync_backup');

my $standby = PostgreSQL::Test::Cluster->new('sync_standby');
$standby->init_from_backup(
	$primary, 'sync_backup',
	has_streaming => 1,
	has_restoring => 1);
my $primary_connstr = $primary->connstr;
$standby->append_conf(
	'postgresql.conf', qq(
checkpoint_timeout = 1h
hot_standby_feedback = on
primary_slot_name = 'sync_phys'
primary_conninfo = '$primary_connstr dbname=postgres'
));
$standby->start;
$primary->wait_for_replay_catchup($standby);

$primary->safe_psql(
	'postgres',
	q{SELECT pg_create_logical_replication_slot(
		'sync_slot', 'pgoutput', false, false, true)});

my $slot_synced = 'f';
foreach (1 .. 10)
{
	$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
	$primary->wait_for_replay_catchup($standby);
	$standby->safe_psql('postgres', 'SELECT pg_sync_replication_slots()');
	$slot_synced = $standby->safe_psql(
		'postgres',
		q{
SELECT count(*) = 1
FROM pg_replication_slots
WHERE slot_name = 'sync_slot'
  AND synced
  AND NOT temporary
  AND invalidation_reason IS NULL
});
	last if $slot_synced eq 't';
}
is($slot_synced, 't', 'valid failover slot is synchronized');
my $sync_restart_lsn = $standby->safe_psql(
	'postgres',
	q{
SELECT restart_lsn
FROM pg_replication_slots
WHERE slot_name = 'sync_slot'
});

$primary->append_conf('postgresql.conf', 'max_slot_wal_keep_size = 1MB');
$primary->reload;
$primary->advance_wal(8);
$primary->wait_for_replay_catchup($standby);
$primary->safe_psql('postgres', 'CHECKPOINT');

is( $primary->safe_psql(
		'postgres',
		q{
SELECT invalidation_reason
FROM pg_replication_slots
WHERE slot_name = 'sync_slot'
}),
	'wal_removed',
	'failover slot is invalidated on the primary');

$standby->safe_psql(
	'postgres',
	q{
SELECT injection_points_attach(
	'replication-slot-save-error', 'error', 'sync_slot')
});

($ret, $stdout, $stderr) =
  $standby->psql('postgres', 'SELECT pg_sync_replication_slots()');
like(
	$stderr,
	qr/error triggered for injection point replication-slot-save-error/,
	'injected error prevents synchronized invalidation from being saved');

is( $standby->safe_psql(
		'postgres',
		q{
SELECT invalidation_reason IS NULL
FROM pg_replication_slots
WHERE slot_name = 'sync_slot'
}),
	't',
	'failed save leaves synchronized slot valid');

$standby->safe_psql('postgres',
	q{SELECT injection_points_detach('replication-slot-save-error')});

$standby->safe_psql('postgres', 'SELECT pg_sync_replication_slots()');

$standby->stop('immediate');
$standby->start;

is( $standby->safe_psql(
		'postgres',
		qq{
SELECT invalidation_reason, restart_lsn = '$sync_restart_lsn'
FROM pg_replication_slots
WHERE slot_name = 'sync_slot'
}),
	'wal_removed|t',
	'retried synchronized invalidation and restart LSN survive restart');

$standby->stop;
$primary->stop;

done_testing();
