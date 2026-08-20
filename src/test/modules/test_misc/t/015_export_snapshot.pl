# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test: reproduce the exported-snapshot xmin handoff race.
#
# Strategy:
#   1. Source exports a snapshot that can still see a tuple deleted later.
#   2. Importer starts a transaction but does not yet import the snapshot, so
#      its xmin is Invalid.
#   3. VACUUM reaches vacuum_get_cutoffs(), then waits after
#      ComputeXidHorizons() has read the importer's still-Invalid xmin.
#   4. Without the fix, SET TRANSACTION SNAPSHOT can complete while VACUUM is
#      paused in the proc-array scan.  The test then commits the source
#      transaction and wakes VACUUM, allowing VACUUM to miss both the importer
#      and the source xmin and remove the deleted tuple.
#   5. With the fix, SET TRANSACTION SNAPSHOT waits for ProcArrayLock
#      exclusive.  The test wakes VACUUM while the source is still open, so
#      VACUUM sees the source xmin before the importer installs the snapshot.
#
# The final query must see the deleted tuple through the imported snapshot.
# On an unfixed server it instead sees zero rows.
#
# Depends only on: injection_points (built-in test module)

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('export_race_node');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

$node->safe_psql('postgres', qq{
	CREATE TABLE race_test (id int, data text);
	INSERT INTO race_test VALUES (1, 'should_be_visible');
});

# Create the importer before the source.  The test does not depend on a fixed
# proc-array index, but the wrong-horizon interleaving requires VACUUM to read
# the importer's Invalid xmin before it reads the source's xmin.
my $imp   = $node->background_psql('postgres');
my $src   = $node->background_psql('postgres');
my $vac   = $node->background_psql('postgres');
my $del   = $node->background_psql('postgres');
my $coord = $node->background_psql('postgres');

my $imp_pid = $imp->query("SELECT pg_backend_pid()");
$imp_pid =~ s/\s+//g;
my $after_reading_importer_ip =
  "compute-xid-horizons-after-reading-pid-$imp_pid";

$src->query("BEGIN ISOLATION LEVEL REPEATABLE READ");
$src->query("SELECT * FROM race_test");
my $token = $src->query("SELECT pg_export_snapshot()");
$token =~ s/\s+//g;
diag("exported snapshot token: $token");

$del->query("DELETE FROM race_test WHERE id = 1");
$del->query("COMMIT");
diag("deleter committed");

# Advance the XID counter so that the horizon (latestCompletedXid + 1 when
# no backend has a valid xmin) is strictly greater than the deleter's XID.
for (my $i = 0; $i < 100; $i++)
{
	$node->safe_psql('postgres', "SELECT txid_current()");
}
diag("100 filler XIDs consumed");

# No query after BEGIN: importer xmin stays Invalid until SET TRANSACTION
# SNAPSHOT, which is essential for this race.
$imp->query("BEGIN ISOLATION LEVEL REPEATABLE READ");

my $vac_pid = $vac->query("SELECT pg_backend_pid()");
$vac_pid =~ s/\s+//g;
diag("VACUUM PID: $vac_pid");

$vac->query("SELECT injection_points_set_local()");
$vac->query(
	"SELECT injection_points_attach('vacuum-get-cutoffs-before-oldest-xmin', 'wait')");
# Attach the per-PID point here, in the VACUUM backend, so that
# injection_points_set_local() tags it with a PID condition.  A globally
# attached point would also fire (and block) in any other process that runs
# ComputeXidHorizons(), e.g. an autovacuum worker, and
# injection_points_wakeup() only wakes the first waiter - leaving the other
# one stuck holding ProcArrayLock SHARED forever.  With the PID condition,
# exactly one waiter can ever exist.
$vac->query(
	"SELECT injection_points_attach('$after_reading_importer_ip', 'wait')");
diag("VACUUM attached both injection points (PID-filtered to VACUUM)");

$vac->query_until(qr/vac_started/,
	"\\echo vac_started\nVACUUM race_test;\n");
diag("VACUUM started");

{
	my $blocked = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND wait_event_type = 'InjectionPoint'"
			  . " AND wait_event = 'vacuum-get-cutoffs-before-oldest-xmin'");
		if ($result =~ /t/)
		{
			$blocked = 1;
			last;
		}
		usleep(100_000);
	}
	die "VACUUM did not reach vacuum_get_cutoffs within 180s"
		unless $blocked;
}
diag("VACUUM blocked before computing VACUUM cutoffs");

$coord->query(
	"SELECT injection_points_detach('vacuum-get-cutoffs-before-oldest-xmin')");
diag("detached vacuum-get-cutoffs-before-oldest-xmin");

$coord->query(
	"SELECT injection_points_wakeup('vacuum-get-cutoffs-before-oldest-xmin')");
diag("woke VACUUM from vacuum_get_cutoffs");

{
	my $blocked = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND wait_event_type = 'InjectionPoint'"
			  . " AND wait_event = '$after_reading_importer_ip'");
		if ($result =~ /t/)
		{
			$blocked = 1;
			last;
		}
		usleep(100_000);
	}
	die "VACUUM did not scan the importer within 180s" unless $blocked;
}
diag("VACUUM blocked after reading importer xmin");

$imp->query_until(qr/import_started/,
	"\\echo import_started\nSET TRANSACTION SNAPSHOT '$token';\n"
	  . "\\echo import_done\n");
diag("importer started SET TRANSACTION SNAPSHOT");

my $importer_waiting = 0;
my $importer_wait_state = '';
for (my $i = 0; $i < 100; $i++)
{
	my $result = $coord->query(
		"SELECT COALESCE(state, '') || '|' ||"
		  . " COALESCE(wait_event_type, '') || '|' ||"
		  . " COALESCE(wait_event, '')"
		  . " FROM pg_stat_activity"
		  . " WHERE pid = $imp_pid");
	$importer_wait_state = $result;
	if ($result =~ /active\|LWLock\|ProcArray/)
	{
		$importer_waiting = 1;
		last;
	}
	last if $result eq '';
	usleep(100_000);
}

if ($importer_waiting)
{
	# Detach before waking.  Detaching does not unblock a backend already
	# waiting in injection_wait(), but once VACUUM is woken it must not be
	# able to re-fire a still-attached point (ComputeXidHorizons() runs again
	# during heap pruning via GlobalVisUpdate) and block a second time in a
	# fresh slot that nobody will wake.
	$coord->query("SELECT injection_points_detach('$after_reading_importer_ip')");
	$coord->query("SELECT injection_points_wakeup('$after_reading_importer_ip')");
	diag("woke VACUUM while source transaction is still open");
	$imp->query_until(qr/import_done/, "");
	$src->query("COMMIT");
}
else
{
	$imp->query_until(qr/import_done/, "");
	$src->query("COMMIT");
	diag("source committed before waking VACUUM");
	$coord->query("SELECT injection_points_detach('$after_reading_importer_ip')");
	$coord->query("SELECT injection_points_wakeup('$after_reading_importer_ip')");
}

ok($importer_waiting,
	"importing transaction waits for ProcArrayLock while VACUUM computes horizons");

{
	my $done = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND state = 'idle'");
		if ($result =~ /t/)
		{
			$done = 1;
			last;
	}
	usleep(100_000);
}
chomp($importer_wait_state);
diag("importer wait state: $importer_wait_state");
	die "VACUUM did not finish within 180s" unless $done;
}
diag("VACUUM finished");

my $count = $imp->query("SELECT count(*) FROM race_test");
$count =~ s/\s+//g;
diag("importer sees $count row(s)");

is($count, 1,
	"imported snapshot still sees the row after concurrent VACUUM")
  or diag("BUG DETECTED: export-snapshot xmin race caused "
		. "premature tuple removal (expected 1 row, got $count)");

$imp->query("COMMIT");

$node->stop;
done_testing();
