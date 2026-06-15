# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that recovery_target_action = pause shuts down the standby at the
# recovery target if hot standby cannot start because of subxid overflow.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize the primary with WAL archiving enabled.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, has_archiving => 1);
$node_primary->start;

# Create a table for the subtransactions to write to.
$node_primary->safe_psql('postgres', 'CREATE TABLE subxid_test (id int)');

# Open a transaction with more than PGPROC_MAX_CACHED_SUBXIDS (64)
# subtransactions and keep it open to force a snapshot overflow.  The INSERT
# (not the SAVEPOINT) is what assigns each subtransaction its XID.
my $bg = $node_primary->background_psql('postgres');
$bg->query_safe('BEGIN');
for my $i (1 .. 70)
{
	$bg->query_safe("SAVEPOINT s$i; INSERT INTO subxid_test VALUES ($i)");
}

# Take a base backup while the transaction's subxid cache is overflowed,
# so the initial standby snapshot is incomplete.
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Log another overflowed standby snapshot and choose a recovery target
# after it.
$node_primary->safe_psql('postgres', 'CHECKPOINT');
my $until_lsn = $node_primary->lsn('write');

# Switch WAL segments so the WAL needed to reach the recovery target can
# be archived.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Create a standby that restores WAL from the archive and pauses at the
# recovery target.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);

# Enable DEBUG1 messages to check the recovery snapshot state.
$node_standby->append_conf(
	'postgresql.conf', qq(
recovery_target_lsn = '$until_lsn'
recovery_target_action = pause
log_min_messages = debug1
));

# Use pg_ctl directly because shutdown at the recovery target returns
# success, but leaves no postmaster.pid for Cluster::start() to read.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--log' => $node_standby->logfile,
		'start',
	]);

# Wait for shutdown or unexpected readiness so the checks below can report
# a failure without waiting for a timeout if the server accepts connections.
$node_standby->wait_for_log(
	qr/database system is shut down|database system is ready to accept/);

my $logfile = slurp_file($node_standby->logfile());

# Verify that recovery snapshots remained pending.
like(
	$logfile,
	qr/recovery snapshot waiting for non-overflowed snapshot/,
	'standby waited for a valid recovery snapshot');

unlike(
	$logfile,
	qr/recovery snapshots are now enabled/,
	'standby never reached STANDBY_SNAPSHOT_READY');

# Verify the reason for shutdown and the shutdown path.
like(
	$logfile,
	qr/recovery cannot pause at the recovery target because hot standby is not active/,
	'standby could not pause because hot standby was inactive');

like(
	$logfile,
	qr/shutdown at recovery target/,
	'standby entered the shutdown-at-recovery-target path');

# The standby must not become ready to accept connections.
unlike(
	$logfile,
	qr/database system is ready to accept/,
	'standby never became ready to accept connections');

# Close the held-open transaction on the primary.
$bg->quit;

# Stop the standby directly in case it is still running, since Cluster.pm
# does not track its PID.  Failure is expected if it has already shut down.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--mode' => 'immediate',
		'stop',
	]);
$node_primary->teardown_node;

done_testing();
