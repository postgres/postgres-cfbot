# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Validate that VACUUM logs explain why dead tuples could not be removed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up a cluster
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', q[
max_prepared_transactions = 1
]);
$node->start;

# Take the backup and initialize the standbys early, before any background
# psql sessions run.  On Windows, terminated background psql sessions can
# leave lingering file handles that make a later pg_ctl start for the standby
# fail; doing it now lets the later start only have to launch pg_ctl.
$node->backup('oldestxmin_hotstandby_bkp');
my $standby = PostgreSQL::Test::Cluster->new('oldestxmin_standby');
$standby->init_from_backup($node, 'oldestxmin_hotstandby_bkp',
	has_streaming => 1);
$standby->append_conf('postgresql.conf', q[
hot_standby_feedback = on
wal_receiver_status_interval = 1s
]);

# A second standby that streams through a physical replication slot, used to
# check that a standby's feedback xmin held on a slot is reported as that
# slot.
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('physical_slot');");
my $slot_standby = PostgreSQL::Test::Cluster->new('oldestxmin_slot_standby');
$slot_standby->init_from_backup($node, 'oldestxmin_hotstandby_bkp',
	has_streaming => 1);
$slot_standby->append_conf('postgresql.conf', q[
primary_slot_name = 'physical_slot'
hot_standby_feedback = on
wal_receiver_status_interval = 1s
]);


#
# Active statement
#
my $active_table = 'blocker_active';
$node->safe_psql('postgres', qq[
CREATE TABLE $active_table(id int);
INSERT INTO $active_table VALUES (0);
]);

my $blocker = $node->background_psql('postgres');
my $blocker_pid = $blocker->query_safe('SELECT pg_backend_pid();');
chomp($blocker_pid);

# Hold a snapshot by selecting from a table; pg_sleep alone takes no
# snapshot, so xmin would stay unset.
$blocker->query_until(qr//, qq[
BEGIN;
SELECT * FROM $active_table, pg_sleep(60);
]);

$node->poll_query_until('postgres', qq[
SELECT backend_xmin IS NOT NULL
FROM pg_stat_activity
WHERE pid = $blocker_pid;
]);

$node->safe_psql('postgres', "DELETE FROM $active_table;");

my $stderr = '';
$node->psql('postgres', "VACUUM (VERBOSE) $active_table;", stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: transaction holding snapshot \(pid = $blocker_pid\)/,
	'VACUUM VERBOSE reported the snapshot holder as oldest xmin blocker');

# Cleanup
$node->safe_psql('postgres', qq[
SELECT pg_terminate_backend($blocker_pid);
DROP TABLE $active_table;
]);


#
# Prefer xid owner over xmin match
#
my $prefer_table = 'blocker_prefer_xid_owner';
$node->safe_psql('postgres', qq[
CREATE TABLE $prefer_table(id int);
INSERT INTO $prefer_table VALUES (0);
]);

my $xid_owner = $node->background_psql('postgres');
my $xid_owner_pid = $xid_owner->query_safe('SELECT pg_backend_pid();');
chomp($xid_owner_pid);

$xid_owner->query_safe(qq[
BEGIN;
SELECT pg_current_xact_id();
]);

$node->poll_query_until('postgres', qq[
SELECT backend_xid IS NOT NULL
FROM pg_stat_activity
WHERE pid = $xid_owner_pid;
]);

my $owner_xid = $node->safe_psql('postgres', qq[
SELECT backend_xid
FROM pg_stat_activity
WHERE pid = $xid_owner_pid;
]);
chomp($owner_xid);

my $xmin_holder = $node->background_psql('postgres');
my $xmin_holder_pid = $xmin_holder->query_safe('SELECT pg_backend_pid();');
chomp($xmin_holder_pid);

# Start a long-running query that will take a snapshot after xid_owner begins
$xmin_holder->query_until(qr//, qq[
BEGIN;
SELECT * FROM $prefer_table, pg_sleep(60);
]);

# Ensure xmin_holder's xmin is held back by xid_owner
$node->poll_query_until('postgres', qq[
SELECT backend_xmin = '$owner_xid'::xid
FROM pg_stat_activity
WHERE pid = $xmin_holder_pid;
]);

$node->safe_psql('postgres', "DELETE FROM $prefer_table;");

$stderr = '';
$node->psql('postgres', "VACUUM (VERBOSE) $prefer_table;", stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: transaction \(pid = $xid_owner_pid\)/,
	'VACUUM VERBOSE preferred xid owner over xmin match');

# Cleanup
$node->safe_psql('postgres', qq[
SELECT pg_terminate_backend($xmin_holder_pid);
SELECT pg_terminate_backend($xid_owner_pid);
DROP TABLE $prefer_table;
]);


#
# Prepared transaction
#
my $prepared_table = 'blocker_prepared';
$node->safe_psql('postgres', qq[
CREATE TABLE $prepared_table(id int);
INSERT INTO $prepared_table VALUES (0);
BEGIN;
PREPARE TRANSACTION 'gx_vacuum_xmin';
]);

$node->safe_psql('postgres', "DELETE FROM $prepared_table;");

$stderr = '';
$node->psql('postgres', "VACUUM (VERBOSE) $prepared_table;", stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: prepared transaction\n/,
	'VACUUM VERBOSE reported prepared transaction as oldest xmin blocker');

# Cleanup
$node->safe_psql('postgres', qq[
ROLLBACK PREPARED 'gx_vacuum_xmin';
DROP TABLE $prepared_table;
]);


#
# Logical replication slot
#
my $slot_table = 'blocker_slot';
$node->safe_psql('postgres', qq[
CREATE TABLE $slot_table(id int);
SELECT pg_create_logical_replication_slot('logical_slot', 'test_decoding');
DROP TABLE $slot_table;
]);

$stderr = '';
$node->psql('postgres', 'VACUUM (VERBOSE) pg_class;', stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: replication slot \(slot name = logical_slot\)/,
	'VACUUM VERBOSE reported logical replication slot by name');

# Cleanup
$node->safe_psql('postgres', qq[
SELECT pg_drop_replication_slot('logical_slot');
]);


#
# Hot standby feedback
#
# The standby was already initialized from a backup taken above.  Start it
# now, after all background psql sessions from earlier tests have been fully
# cleaned up.
my $hs_table = 'blocker_hotstandby';
$node->safe_psql('postgres', qq[
CREATE TABLE $hs_table(id int);
INSERT INTO $hs_table VALUES (0);
]);

$standby->start;
$node->wait_for_replay_catchup($standby);

my $standby_reader = $standby->background_psql('postgres');
my $standby_reader_pid = $standby_reader->query_safe('SELECT pg_backend_pid();');
chomp($standby_reader_pid);

$standby_reader->query_until(qr//, qq[
BEGIN;
SELECT * FROM $hs_table, pg_sleep(60);
]);

# Establish the reader's snapshot on the standby and capture its xmin.  The
# DELETE below must use a newer xid than this so the deleted tuple stays
# "recently dead".  Waiting for the feedback to actually carry the reader's
# xmin (rather than merely being non-null) avoids racing a periodic feedback
# message that predates the reader's snapshot.
$standby->poll_query_until('postgres', qq[
SELECT backend_xmin IS NOT NULL
FROM pg_stat_activity
WHERE pid = $standby_reader_pid;
]);
my $reader_xmin = $standby->safe_psql('postgres', qq[
SELECT backend_xmin FROM pg_stat_activity WHERE pid = $standby_reader_pid;
]);

# Wait for hot standby feedback carrying the reader's xmin to reach the primary
$node->poll_query_until('postgres', qq[
SELECT backend_xmin = '$reader_xmin'::xid
FROM pg_stat_replication
WHERE application_name = 'oldestxmin_standby';
]);

my $hs_blocker_pid = $node->safe_psql('postgres', q[
SELECT pid FROM pg_stat_replication
WHERE application_name = 'oldestxmin_standby';
]);
chomp($hs_blocker_pid);

$node->safe_psql('postgres', "DELETE FROM $hs_table;");

$stderr = '';
$node->psql('postgres', "VACUUM (VERBOSE) $hs_table;", stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: hot standby feedback \(pid = $hs_blocker_pid\)/,
	'VACUUM VERBOSE reported hot standby feedback as oldest xmin blocker');

# Cleanup
$standby->safe_psql('postgres', "SELECT pg_terminate_backend($standby_reader_pid);");
$node->safe_psql('postgres', "DROP TABLE $hs_table;");
$standby->stop;


#
# Physical replication slot
#
# When a standby streams through a physical slot, the feedback xmin is held
# by the slot rather than the walsender's PGPROC, and the slot is reported by
# name.
#
my $slot_hs_table = 'blocker_slot_hotstandby';
$node->safe_psql('postgres', qq[
CREATE TABLE $slot_hs_table(id int);
INSERT INTO $slot_hs_table VALUES (0);
]);

$slot_standby->start;
$node->wait_for_replay_catchup($slot_standby);

my $slot_reader = $slot_standby->background_psql('postgres');
my $slot_reader_pid = $slot_reader->query_safe('SELECT pg_backend_pid();');
chomp($slot_reader_pid);

$slot_reader->query_until(qr//, qq[
BEGIN;
SELECT * FROM $slot_hs_table, pg_sleep(60);
]);

# Establish the reader's snapshot on the standby and capture its xmin.  When a
# physical slot is used the feedback xmin is held on the slot rather than in
# the walsender's PGPROC, so pg_stat_replication.backend_xmin stays null here;
# wait on the slot's xmin instead, requiring it to carry the reader's xmin.
$slot_standby->poll_query_until('postgres', qq[
SELECT backend_xmin IS NOT NULL
FROM pg_stat_activity
WHERE pid = $slot_reader_pid;
]);
my $slot_reader_xmin = $slot_standby->safe_psql('postgres', qq[
SELECT backend_xmin FROM pg_stat_activity WHERE pid = $slot_reader_pid;
]);

$node->poll_query_until('postgres', qq[
SELECT xmin = '$slot_reader_xmin'::xid
FROM pg_replication_slots
WHERE slot_name = 'physical_slot';
]);


# Disconnect the standby, so that the slot reserves the xmin on its own, with
# no walsender holding it.
#
# On Windows this slot-using standby can be slow to fully shut down, so pg_ctl
# may spuriously report "server does not shut down"; tolerate it with fail_ok.
# This is safe: the primary's walsender releases the slot as soon as the
# connection drops, and stop() still detects the node is actually down.
$slot_standby->safe_psql('postgres',
	"SELECT pg_terminate_backend($slot_reader_pid);");
$slot_standby->stop('fast', fail_ok => 1);

# Wait for the walsender to exit so the slot is no longer active.
$node->poll_query_until('postgres', q[
SELECT NOT EXISTS (
  SELECT 1 FROM pg_stat_replication
  WHERE application_name = 'oldestxmin_slot_standby')
AND NOT (SELECT active FROM pg_replication_slots
         WHERE slot_name = 'physical_slot');
]);

# The slot's xmin is frozen now, so a tuple deleted with a newer xid stays
# "recently dead" for VACUUM to report on.
$node->safe_psql('postgres', qq[
INSERT INTO $slot_hs_table VALUES (1);
DELETE FROM $slot_hs_table;
]);

$stderr = '';
$node->psql('postgres', "VACUUM (VERBOSE) $slot_hs_table;", stderr => \$stderr);
like(
	$stderr,
	qr/removable cutoff was held back by: replication slot \(slot name = physical_slot\)/,
	'VACUUM VERBOSE reported the slot of a disconnected standby by name');

# Cleanup
$node->safe_psql('postgres', qq[
DROP TABLE $slot_hs_table;
SELECT pg_drop_replication_slot('physical_slot');
]);


$node->stop;
done_testing();
