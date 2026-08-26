# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test concurrent invalidation of the same replication slot.
#
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(usleep);

use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$primary->append_conf(
	'postgresql.conf', qq(
wal_level = logical
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 64MB
));
$primary->start;

if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('phys')});
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
primary_slot_name = 'phys'
hot_standby_feedback = on
checkpoint_timeout = 1h
max_wal_size = 64MB
max_slot_wal_keep_size = 1MB
log_checkpoints = on
));
$standby->start;
$primary->wait_for_replay_catchup($standby);

my $injection_point = 'replication-slot-save-error';

sub set_primary_wal_level
{
	my ($wal_level) = @_;

	$primary->append_conf('postgresql.conf', "wal_level = $wal_level");
	$primary->restart;
}

sub create_lagging_slot
{
	my ($slot_name) = @_;

	$standby->create_logical_slot_on_standby($primary, $slot_name,
		'postgres');
	$primary->advance_wal(8);
	$primary->safe_psql('postgres', 'CHECKPOINT');
	$primary->wait_for_replay_catchup($standby);
	$standby->safe_psql(
		'postgres',
		qq{
SELECT injection_points_attach(
	'$injection_point', 'wait', '$slot_name')
});
}

sub backend_pid
{
	my ($backend_type) = @_;

	return $standby->safe_psql(
		'postgres',
		qq{
SELECT pid
FROM pg_stat_activity
WHERE backend_type = '$backend_type'
});
}

sub start_restartpoint
{
	my $checkpoint =
	  $standby->background_psql('postgres', on_error_stop => 0);

	$checkpoint->set_query_timer_restart();
	$checkpoint->query_until(
		qr/checkpoint started/,
		q(\echo checkpoint started
CHECKPOINT;
));

	return $checkpoint;
}

sub finish_restartpoint
{
	my ($checkpoint) = @_;
	my (undef, $error) = $checkpoint->query('SELECT 1', verbose => 0);

	is($error, 0, 'restartpoint succeeds');
	$checkpoint->quit;
}

sub wait_for_replication_slot_io
{
	my ($pid) = @_;

	$standby->poll_query_until(
		'postgres',
		qq{
SELECT wait_event = 'ReplicationSlotIO'
FROM pg_stat_activity
WHERE pid = $pid
}) or die "process $pid did not wait for replication slot I/O";
}

sub wake_invalidator
{
	my ($slot_name) = @_;
	my $invalidation_reason;

	foreach (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		my $waiting = $standby->safe_psql(
			'postgres',
			qq{
SELECT count(*) > 0
FROM pg_stat_activity
WHERE wait_event = '$injection_point'
});

		$standby->safe_psql('postgres',
			qq{SELECT injection_points_wakeup('$injection_point')})
		  if $waiting eq 't';

		$invalidation_reason = $standby->safe_psql(
			'postgres',
			qq{
SELECT invalidation_reason
FROM pg_replication_slots
WHERE slot_name = '$slot_name'
});

		last if $invalidation_reason ne '' && $waiting eq 'f';
		usleep(100_000);
	}

	die "timed out waiting for slot $slot_name to be invalidated"
	  if !defined($invalidation_reason) || $invalidation_reason eq '';

	return $invalidation_reason;
}

# The checkpointer starts invalidation before the startup process.
my $slot_name = 'checkpointer_first';
create_lagging_slot($slot_name);
my $startup_pid = backend_pid('startup');
my $checkpointer_pid = backend_pid('checkpointer');
my $log_start = -s $standby->logfile;

my $checkpoint = start_restartpoint();
$standby->wait_for_event('checkpointer', $injection_point);

is( $standby->safe_psql(
		'postgres',
		qq{
SELECT active_pid = $checkpointer_pid
FROM pg_replication_slots
WHERE slot_name = '$slot_name'
}),
	't',
	'checkpointer owns the slot while persisting invalidation');

set_primary_wal_level('replica');
wait_for_replication_slot_io($startup_pid);

ok( !$standby->log_contains(
		qr/terminating process $checkpointer_pid to release replication slot
		   \s+"$slot_name"/x,
		$log_start),
	'startup process does not terminate the checkpointer');

my $invalidation_reason = wake_invalidator($slot_name);
finish_restartpoint($checkpoint);

is($invalidation_reason, 'wal_removed',
	'slot is invalidated by the checkpointer');
ok( !$standby->log_contains(
		qr/canceling statement due to conflict with recovery/, $log_start),
	'checkpointer does not receive a recovery conflict');

$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres',
	qq{SELECT injection_points_detach('$injection_point')});

# The startup process starts invalidation before the checkpointer.
set_primary_wal_level('logical');
$primary->wait_for_replay_catchup($standby);

$slot_name = 'startup_first';
create_lagging_slot($slot_name);
$startup_pid = backend_pid('startup');
$checkpointer_pid = backend_pid('checkpointer');
$log_start = -s $standby->logfile;

set_primary_wal_level('replica');
$standby->wait_for_event('startup', $injection_point);

is( $standby->safe_psql(
		'postgres',
		qq{
SELECT active_pid = $startup_pid
FROM pg_replication_slots
WHERE slot_name = '$slot_name'
}),
	't',
	'startup process owns the slot while persisting invalidation');

$checkpoint = start_restartpoint();
wait_for_replication_slot_io($checkpointer_pid);

ok( !$standby->log_contains(
		qr/terminating process $startup_pid to release replication slot
		   \s+"$slot_name"/x,
		$log_start),
	'checkpointer does not terminate the startup process');

$standby->safe_psql('postgres',
	qq{SELECT injection_points_wakeup('$injection_point')});
finish_restartpoint($checkpoint);
$standby->wait_for_log(
	qr/invalidating obsolete replication slot "$slot_name"/, $log_start);

$primary->advance_wal(1);
$primary->wait_for_replay_catchup($standby);
is(backend_pid('startup'), $startup_pid, 'startup process survives');
ok($standby->is_alive, 'standby remains running');

if ($standby->is_alive)
{
	is( $standby->safe_psql(
			'postgres',
			qq{
SELECT invalidation_reason
FROM pg_replication_slots
WHERE slot_name = '$slot_name'
}),
		'wal_level_insufficient',
		'slot is invalidated by the startup process');

	$standby->safe_psql('postgres',
		qq{SELECT injection_points_detach('$injection_point')});

	$standby->stop;
}

$primary->stop;

done_testing();
