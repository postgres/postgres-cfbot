
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test synchronized_standby_slots with different syntax modes:
# - Plain list (ALL mode): slot1, slot2
# - ANY N (quorum mode): ANY N (slot1, slot2, ...)
#
# Setup: a 3-node cluster with one primary, two physical standbys, and a
# logical decoding client using a failover-enabled slot.
#
#               | ----> standby1 (primary_slot_name = sb1_slot)
# primary ------|
#               | ----> standby2 (primary_slot_name = sb2_slot)
#
#   synchronous_standby_names = 'ANY 1 (standby1, standby2)'
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------------
# 1.  Create a primary with logical replication level, autovacuum off
# ---------------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical');
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
});
$primary->start;

# Physical replication slots for the two standbys
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb1_slot');");
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb2_slot');");

# ---------------------------------------------------------------------------
# 2.  Create standby1 and standby2 from a fresh backup
# ---------------------------------------------------------------------------
my $backup_name = 'base_backup';
$primary->backup($backup_name);

my $connstr = $primary->connstr;

my $standby1 = PostgreSQL::Test::Cluster->new('standby1');
$standby1->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$standby1->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'sb1_slot'
primary_conninfo = '$connstr dbname=postgres'
));

my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$standby2->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'sb2_slot'
primary_conninfo = '$connstr dbname=postgres'
));

$standby1->start;
$standby2->start;

$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

# ---------------------------------------------------------------------------
# 3.  Create a logical failover slot on the primary
# ---------------------------------------------------------------------------
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('logical_failover', 'test_decoding', false, false, true);"
);

# ---------------------------------------------------------------------------
# 4.  Configure quorum sync rep with ALL-mode synchronized_standby_slots
# ---------------------------------------------------------------------------
$primary->append_conf(
	'postgresql.conf', qq{
synchronous_standby_names = 'ANY 1 (standby1, standby2)'
synchronized_standby_slots = 'sb1_slot, sb2_slot'
});
$primary->reload;

$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

# ---------------------------------------------------------------------------
# 5.  Confirm that quorum sync rep is active for both standbys
# ---------------------------------------------------------------------------
is( $primary->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_stat_replication WHERE sync_state = 'quorum';}
	),
	'2',
	'both standbys are in quorum sync state');

##################################################
# PART A: Plain list (ALL mode) blocks when any slot is unavailable
##################################################

$standby1->stop;

# Commit succeeds since standby2 satisfies the quorum.
my $emit_lsn = $primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'all_mode_blocks');"
);
like($emit_lsn, qr/^[0-9A-F]+\/[0-9A-F]+$/,
	'synchronous commit succeeds with quorum (standby2 alive)');

$primary->wait_for_replay_catchup($standby2);

my $log_offset = -s $primary->logfile;

my $bg = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg->query_until(
	qr/decode_start/, q(
   \echo decode_start
   SELECT pg_logical_slot_peek_changes('logical_failover', NULL, NULL);
));

# Wait for the primary to log a warning about sb1_slot not being active.
$primary->wait_for_log(
	qr/replication slot \"sb1_slot\" specified in parameter "synchronized_standby_slots" does not have active_pid/,
	$log_offset);

pass('plain list (ALL mode): logical decoding blocked by unavailable sb1_slot');

# Unblock by clearing synchronized_standby_slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg->quit;

# Consume the change so the slot is clean for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

##################################################
# PART B: ANY mode (quorum) — logical decoding proceeds with N-of-M slots
##################################################

# Switch synchronized_standby_slots to quorum mode: need only 1 of 2 slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'ANY 1 (sb1_slot, sb2_slot)'");
$primary->reload;

# standby1 is still down; standby2 is up.

# Emit another transactional message — commits via quorum.
$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'quorum_mode_works');"
);
$primary->wait_for_replay_catchup($standby2);

# In quorum mode, logical decoding should NOT block because sb2_slot has
# caught up and 1-of-2 is sufficient.
my $decoded = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%quorum_mode_works%';});
is($decoded, '1',
	'ANY mode: logical decoding proceeds with only sb2_slot caught up');

##################################################
# PART C: Re-check plain list (ALL mode) works when both standbys are up
##################################################

# Bring standby1 back.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);

# Switch to plain list (ALL mode) with both slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'sb1_slot, sb2_slot'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'both_caught_up');"
);
$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

my $decoded_bc = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%both_caught_up%';});
is($decoded_bc, '1',
	'plain list: works when all standbys are up');

##################################################
# PART D: ANY 2 waits on an active lagging slot
##################################################

# Stop standby1 so sb1_slot can be controlled by a raw replication connection
# that keeps the slot active while lagging.
$standby1->stop;

my $old_lsn = $primary->safe_psql('postgres',
	"SELECT pg_current_wal_lsn();");

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'ANY 2 (sb1_slot, sb2_slot)'");
$primary->reload;

my $any2_lag_lsn = $primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'any_2_lagging_blocks');"
);
$primary->wait_for_replay_catchup($standby2);

my $repl_any2 = $primary->background_psql(
	'postgres',
	replication => 'database',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$repl_any2->query_until(
	qr/^$/,
	"START_REPLICATION SLOT sb1_slot PHYSICAL $old_lsn;\n");

$primary->poll_query_until('postgres', q{
	SELECT active_pid IS NOT NULL
	FROM pg_replication_slots
	WHERE slot_name = 'sb1_slot'
}) or die "replication connection did not activate sb1_slot";

my $bg_any2 = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg_any2->query_until(
	qr/decode_start/, q(
   \echo decode_start
   SELECT pg_logical_slot_peek_changes('logical_failover', NULL, NULL);
));

ok( $primary->poll_query_until(
		'postgres', q{
SELECT EXISTS (
	SELECT 1
	FROM pg_stat_activity
	WHERE wait_event = 'WaitForStandbyConfirmation'
	  AND query LIKE '%pg_logical_slot_peek_changes(''logical_failover''%'
);
}),
	'ANY 2: decoding waits when only one slot has caught up');

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg_any2->quit;
$repl_any2->quit;

# Consume the change for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

# Bring standby1 back up for the remaining tests.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);


##################################################
# PART E: Duplicate entries are ignored for quorum counting
##################################################

# Stop standby2 so only sb1_slot can catch up.
$standby2->stop;

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'ANY 2 (sb1_slot, sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'duplicate_entries_ignored');"
);
$primary->wait_for_replay_catchup($standby1);

my $bg_dup = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg_dup->query_until(
	qr/decode_start/, q(
   \echo decode_start
   SELECT pg_logical_slot_peek_changes('logical_failover', NULL, NULL);
));

ok( $primary->poll_query_until(
		'postgres', q{
SELECT EXISTS (
	SELECT 1
	FROM pg_stat_activity
	WHERE wait_event = 'WaitForStandbyConfirmation'
	  AND query LIKE '%pg_logical_slot_peek_changes(''logical_failover''%'
);
}),
	'duplicate entries are ignored when counting quorum slots');

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg_dup->quit;

# Consume the change for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

# Bring standby2 back up for validation tests.
$standby2->start;
$primary->wait_for_replay_catchup($standby2);


##################################################
# PART F: Verify GUC validation rejects bad values
##################################################

my ($result, $stdout, $stderr);

# N exceeds number of listed slots
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 3 (sb1_slot, sb2_slot)';");
like($stderr, qr/ERROR/,
	'GUC rejects ANY N when N > number of listed slots');

# Missing closing parenthesis
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 1 (sb1_slot, sb2_slot';");
like($stderr, qr/ERROR/,
	'GUC rejects malformed ANY syntax');

# Priority syntax is not supported by synchronized_standby_slots yet
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'FIRST 1 (sb1_slot, sb2_slot)';");
like($stderr, qr/priority syntax is not supported/,
	'GUC rejects FIRST syntax');

# Legacy priority syntax is not supported by synchronized_standby_slots yet
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = '1 (sb1_slot, sb2_slot)';");
like($stderr, qr/priority syntax is not supported/,
	'GUC rejects legacy priority syntax');

# Invalid slot name
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 1 (INVALID_UPPER)';");
like($stderr, qr/ERROR/,
	'GUC rejects invalid slot name in ANY syntax');

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
$primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('logical_failover');");

done_testing();
