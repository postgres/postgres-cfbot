# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that a standby reaching a recovery target with
# 'recovery_target_action = pause' shuts down, rather than silently
# promoting, when hot standby never became active because the standby
# snapshot was still incomplete (STANDBY_SNAPSHOT_PENDING).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, has_archiving => 1);
$node_primary->start;

# Create a table to use by the subtransactions.
$node_primary->safe_psql('postgres', "CREATE TABLE subxid_test (id int);");

# Open a transaction with more than PGPROC_MAX_CACHED_SUBXIDS (64)
# subtransactions and keep it open to force a snapshot overflow.  The INSERT
# (not the SAVEPOINT) is what assigns each subtransaction its XID.
my $bg = $node_primary->background_psql('postgres');
$bg->query_safe('BEGIN');
for my $i (1 .. 70)
{
	$bg->query_safe("SAVEPOINT s$i");
	$bg->query_safe("INSERT INTO subxid_test VALUES ($i)");
}

# Take a base backup.
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Perform a checkpoint and record the LSN as recovery target.
$node_primary->safe_psql('postgres', 'CHECKPOINT');
my $until_lsn =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

# Force a segment switch so the WAL segment containing the recovery target is
# archived and reachable through the standby's restore_command.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Create an archive-recovery standby that is configured to pause once it
# reaches the target.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);
$node_standby->append_conf('postgresql.conf',
	"recovery_target_lsn = '$until_lsn'");
$node_standby->append_conf('postgresql.conf',
	'recovery_target_action = pause');
# Raise the log level so we can observe that the standby kept waiting for a
# non-overflowed snapshot.
$node_standby->append_conf('postgresql.conf', 'log_min_messages = debug1');

# The standby reaches the recovery target but, because every snapshot it saw
# was overflowed, it could never enable hot standby.  It must therefore refuse
# to pause and shut down instead of promoting.  The server stops on its own, so
# we drive it with pg_ctl directly (a regular ->start() would error out because
# the server never becomes ready to accept connections).
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--log' => $node_standby->logfile,
		'start',
	]);

# Wait for the standby to resolve the recovery target.  It must shut down; a
# regressed fix would instead promote and open the server for connections.
$node_standby->wait_for_log(
	qr/database system is shut down|database system is ready to accept/);

my $logfile = slurp_file($node_standby->logfile());

# The standby actually exercised the overflowed-snapshot path...
like(
	$logfile,
	qr/recovery snapshot waiting for non-overflowed snapshot/,
	'standby kept waiting for a non-overflowed snapshot');

# ... and never reached STANDBY_SNAPSHOT_READY.
unlike(
	$logfile,
	qr/recovery snapshots are now enabled/,
	'standby never reached a ready (non-overflowed) snapshot');

# It must have shut down for the documented reason ...
like(
	$logfile,
	qr/recovery cannot pause at the recovery target because hot standby is not active/,
	'standby refused to pause without hot standby');

# ... by taking the clean shutdown-at-recovery-target path (not a crash) ...
like(
	$logfile,
	qr/shutdown at recovery target/,
	'standby shut down cleanly at the recovery target');

# ... and never opened up for connections (neither as a hot standby nor by
# promoting), which is what would have allowed unsafe queries.
unlike(
	$logfile,
	qr/database system is ready to accept/,
	'standby never accepted connections');

# Close the held-open transaction on the primary.
$bg->quit;

$node_standby->teardown_node;
$node_primary->teardown_node;

done_testing();
