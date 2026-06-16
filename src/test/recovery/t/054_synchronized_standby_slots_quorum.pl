
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test synchronized_standby_slots with different syntax modes:
# - Plain list (ALL mode): slot1, slot2
# - ANY N (quorum mode): ANY N (slot1, slot2, ...)
# - FIRST N (priority mode): FIRST N (slot1, slot2, ...)
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
# PART D: Verify FIRST N priority semantics
##################################################

# FIRST N should:
# 1. Select first N slots in priority order (list order)
# 2. Skip missing/invalid/logical slots and inactive lagging slots to find
#    N caught-up slots
# 3. Wait for active lagging slots (not skip to lower priority)

# Test FIRST 2 (sb1_slot, sb2_slot) with both up; should wait for both.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'FIRST 2 (sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'first_2_both_up');"
);
$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

my $decoded_e2 = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%first_2_both_up%';});
is($decoded_e2, '1',
	'FIRST 2: decoding works when all required slots are up');

# Test FIRST 1 (sb1_slot, sb2_slot) with sb1_slot unavailable.
$standby1->stop;

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'FIRST 1 (sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'first_1_skip_unavailable');"
);
$primary->wait_for_replay_catchup($standby2);

# FIRST 1 should skip sb1_slot (unavailable) and use sb2_slot.
my $decoded_e1 = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%first_1_skip_unavailable%';});
is($decoded_e1, '1',
	'FIRST 1: skips unavailable first slot, uses second slot');

# Test shorthand priority syntax: N (...) means FIRST N (...).
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'1 (sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'num_1_shorthand_priority');"
);
$primary->wait_for_replay_catchup($standby2);

my $decoded_num1 = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%num_1_shorthand_priority%';});
is($decoded_num1, '1',
	'1 (...): shorthand priority syntax behaves like FIRST 1');

##################################################
# PART E: FIRST 1 and ANY 2 wait on an active lagging slot
##################################################

# Bring standby1 back so sb1_slot is active and caught up.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);

# To test the active-but-lagging slot path deterministically, we open a raw
# replication connection to sb1_slot starting from a deliberately old LSN.
# psql in replication mode never sends Standby Status Update messages, so
# the walsender keeps sb1_slot's active_pid set but restart_lsn never
# advances.

# Stop standby1 so its walsender releases sb1_slot, allowing our replication
# connection below to acquire it.
$standby1->stop;

# Capture a safely old LSN to stream from, before the test WAL record.
my $old_lsn = $primary->safe_psql('postgres',
	"SELECT pg_current_wal_lsn();");

# FIRST 1 must wait for the highest-priority slot when it is active but lagging.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'FIRST 1 (sb1_slot, sb2_slot)'");
$primary->reload;

my $first_lag_lsn = $primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'first_1_lagging_blocks');"
);
$primary->wait_for_replay_catchup($standby2);

# Open a raw replication connection to sb1_slot starting from $old_lsn.
# This activates the slot (active_pid IS NOT NULL) while keeping restart_lsn
# frozen below $first_lag_lsn for the lifetime of the connection.
my $repl_first = $primary->background_psql(
	'postgres',
	replication => 'database',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$repl_first->query_until(
	qr/^$/,
	"START_REPLICATION SLOT sb1_slot PHYSICAL $old_lsn;\n");

# Wait until sb1_slot shows active_pid, confirming the walsender is live.
$primary->poll_query_until('postgres', q{
	SELECT active_pid IS NOT NULL
	FROM pg_replication_slots
	WHERE slot_name = 'sb1_slot'
}) or die "replication connection did not activate sb1_slot";

# sb1_slot is now active and its restart_lsn is behind $first_lag_lsn.
# Start logical decoding in the background; it must block.
my $bg_first = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg_first->query_until(
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
	'FIRST 1: decoding waits for active lagging higher-priority slot');

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg_first->quit;
$repl_first->quit;

# Ensure the previous replication connection has fully released sb1_slot
# before reusing it in the next subtest.
$primary->poll_query_until('postgres', q{
	SELECT active_pid IS NULL
	FROM pg_replication_slots
	WHERE slot_name = 'sb1_slot'
}) or die "replication connection did not release sb1_slot";

# Consume the change so the slot is clean for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

# ANY 2 must also wait when only one of two required slots has caught up.
# Reuse the same technique: open a raw replication connection to sb1_slot
# from $old_lsn so it is active but its restart_lsn stays behind the target.

# Capture another old LSN baseline before the next test WAL record.
$old_lsn = $primary->safe_psql('postgres',
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
# PART F: Duplicate entries are ignored for quorum counting
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

# FIRST duplicates must also not create extra priority positions.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'FIRST 2 (sb1_slot, sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'first_duplicate_entries_ignored');"
);
$primary->wait_for_replay_catchup($standby1);

my $bg_first_dup = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg_first_dup->query_until(
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
	'FIRST duplicates are ignored when counting priority slots');

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg_first_dup->quit;

# Consume the change for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

# Bring standby2 back up for validation tests.
$standby2->start;
$primary->wait_for_replay_catchup($standby2);


##################################################
# PART G: Verify GUC validation rejects bad values
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
