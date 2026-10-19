# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a standby with hot_standby_feedback = on and
# wal_receiver_status_interval = 0 does not leave a stale xmin on the primary.
#
# With the interval set to zero, the walreceiver sends hot standby feedback
# at most once after a (re)configuration or a connection.  If that message
# carries a valid xmin, the primary keeps it until the next message, which
# never comes, and it holds back vacuum.  So the walreceiver must send an
# invalid xmin in this case.
#
# A physical replication slot is used to make the xmin visible on the primary
# in pg_replication_slots.xmin, as that is what holds back vacuum and it
# survives a disconnection.  The first check is a control with a positive
# interval, and it only waits for the slot to get an xmin.  Each check with a
# zero interval has two parts:
#
# - The DEBUG2 message that the walreceiver logs for each feedback message it
#   sends, which does not depend on when the primary processes the message.
# - The slot xmin on the primary.  The xmin is valid before the step under
#   test, so we poll until it becomes NULL.  This never passes early, and it
#   times out if a stale xmin is left behind.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->backup('bkp');
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb_slot')");

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
primary_slot_name = 'sb_slot'
hot_standby_feedback = on
wal_receiver_status_interval = 1
wal_receiver_timeout = 0
wal_retrieve_retry_interval = 100ms
log_min_messages = 'warning, walreceiver:debug2'
));
$standby->start;

my $slot_xmin_set = "SELECT xmin IS NOT NULL FROM pg_replication_slots
	 WHERE slot_name = 'sb_slot'";
my $slot_xmin_null = "SELECT xmin IS NULL FROM pg_replication_slots
	 WHERE slot_name = 'sb_slot'";
my $feedback_no_xmin =
  qr/sending hot standby feedback xmin 0 epoch \d+ catalog_xmin 0 catalog_xmin_epoch \d+/;

# Control: with a positive interval, feedback with a valid xmin is sent and
# the slot gets an xmin.
$primary->poll_query_until('postgres', $slot_xmin_set)
  or die "timed out waiting for the slot to get the standby xmin";
pass('slot gets the standby xmin with wal_receiver_status_interval > 0');

# Changing the interval to zero on reload must release the xmin.  The
# walreceiver sends feedback right away on a configuration reload.
my $log_offset = -s $standby->logfile;
$standby->append_conf('postgresql.conf', "wal_receiver_status_interval = 0");
$standby->reload;
$standby->wait_for_log($feedback_no_xmin, $log_offset);
pass('feedback without xmin is sent when the interval is set to 0');
$primary->poll_query_until('postgres', $slot_xmin_null)
  or die "timed out waiting for the slot xmin to be released";
pass('slot xmin is released when the interval is set to 0');

# Now check the feedback sent at connection time.  Get a valid xmin on the
# slot again, then restart the standby with the interval set to zero.  The
# slot keeps the old xmin while the standby is down.
$standby->append_conf('postgresql.conf', "wal_receiver_status_interval = 1");
$standby->reload;
$primary->poll_query_until('postgres', $slot_xmin_set)
  or die "timed out waiting for the slot to get the standby xmin again";
$standby->stop;
$standby->append_conf('postgresql.conf', "wal_receiver_status_interval = 0");
$standby->start;

# The first feedback message is sent as soon as the walreceiver connects, and
# at that time Hot Standby may not be active yet, in which case nothing is
# sent.  Make the walreceiver connect again now that the standby accepts
# connections, so that the feedback computed at connection time is always the
# one that is checked.
$log_offset = -s $standby->logfile;
$primary->safe_psql('postgres',
	"SELECT pg_terminate_backend(pid) FROM pg_stat_replication");
$standby->wait_for_log($feedback_no_xmin, $log_offset);
pass('feedback sent at connection has no xmin with wal_receiver_status_interval = 0');
$primary->poll_query_until('postgres', $slot_xmin_null)
  or die "timed out waiting for the slot xmin to be released";
pass('slot has no stale xmin after connecting with wal_receiver_status_interval = 0');

$standby->stop;
$primary->stop;

done_testing();
