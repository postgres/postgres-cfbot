# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: ownership across ProcNumber reuse (fix 2).
#
# The always-resident control slot for a ProcNumber records
# owner_pid/owner_start alongside the DSA payload pointer, and every
# reader compares them against the live PgBackendStatus entry before
# trusting the payload, so a successor never gets attributed a
# predecessor's counters, even before it has attached its own payload.
# This test quits a capturing backend and then hunts for a successor
# that reused its ProcNumber, to drive that comparison for real, and
# checks both readers -- a superuser, and the backend reading about
# itself -- see the right thing at each step.
#
# PGPROC's free list is FIFO, not LIFO: InitProcess() pops the head
# (src/backend/storage/lmgr/proc.c) and ProcKill() pushes to the tail,
# so a freed ProcNumber is only handed out again once every other free
# slot has been used first.  max_connections is kept small here so that
# "every other free slot" is a short list, and B is found by opening
# candidate connections in a loop -- each checks its own ProcNumber and
# is dropped if it isn't the one being waited for -- up to a generous,
# bounded number of attempts.
#
# B's own activity below never uses pg_sleep(): pg_sleep() loops,
# calling WaitLatch again until its own clock says the requested time
# is up, and on some platforms (seen on Windows in CI) the latch
# timeout and that clock can disagree, so it loops and records more
# than one wait for a single call. The module is right to count every
# one of them, but that makes "exactly one PgSleep wait" an unsafe
# thing to assert, so B is driven with plain statements instead and
# checked only for "at least one row" and "no PgSleep row at all".

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $max_connections = 10;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(auth_extra => ['--create-role', 'regress_a,regress_b']);
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_wait_event_tracing'");
$node->append_conf('postgresql.conf', "max_connections = $max_connections");
# Keep pg_sleep() running in the session that issued it, not a parallel
# worker, so its wait is recorded under the pid this test is watching.
$node->append_conf('postgresql.conf', "debug_parallel_query = off");
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE ROLE regress_a LOGIN;
CREATE ROLE regress_b LOGIN;
-- pg_wait_event_tracing.capture is PGC_SUSET, so a non-superuser needs
-- an explicit SET grant to toggle it; both roles do below.
GRANT SET ON PARAMETER pg_wait_event_tracing.capture TO regress_a, regress_b;
));

# Session A: one recorded wait, then note its pid and ProcNumber.
my $A = $node->background_psql(
	'postgres',
	connstr => $node->connstr('postgres') . ' user=regress_a');
$A->query_safe("SET pg_wait_event_tracing.capture = stats;");
$A->query_safe("SELECT pg_sleep(0.01);");
my $a_pid = $A->query_safe("SELECT pg_backend_pid();");
my $a_procnumber = $node->safe_psql(
	'postgres',
	"SELECT procnumber FROM pg_stat_wait_event_timing "
	  . "WHERE pid = $a_pid AND wait_event = 'PgSleep';");

$A->quit;
$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $a_pid);"
) or die "backend $a_pid did not disappear from pg_stat_activity";

# Open candidate regress_b connections, one at a time, until one lands
# on A's ProcNumber -- pg_stat_get_backend_idset()'s id is the same
# proc_number the module's own "procnumber" column reports -- or the
# budget below is exhausted.  A candidate that isn't the one wanted is
# dropped immediately, before enabling capture, so it does not itself
# perturb the free list any more than opening and closing one
# connection already does.
my $B;
my $attempts = 0;
my $max_attempts = 3 * $max_connections;
while ($attempts < $max_attempts)
{
	$attempts++;
	my $candidate = $node->background_psql(
		'postgres',
		connstr => $node->connstr('postgres') . ' user=regress_b');
	my $candidate_procnumber = $candidate->query_safe(
		"SELECT id FROM pg_stat_get_backend_idset() AS id "
		  . "WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();");
	if ($candidate_procnumber eq $a_procnumber)
	{
		$B = $candidate;
		last;
	}
	$candidate->quit;
}

SKIP:
{
	skip "ProcNumber $a_procnumber was not reused by any of $attempts "
	  . "regress_b connections; cannot exercise the reuse path in this run",
	  9
	  unless defined $B;

	my $b_pid = $B->query_safe("SELECT pg_backend_pid();");

	# B has not enabled capture yet.
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_stat_wait_event_timing WHERE pid = $b_pid;"
		),
		'0',
		"B has no timing rows before enabling capture");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_stat_wait_event_timing_overflow "
			  . "WHERE pid = $b_pid;"
		),
		'0',
		"B has no overflow rows before enabling capture");

	# Now B enables capture.  Two trivial statements: the first attaches
	# (post_parse_analyze_hook picks it up; the SET's own assign hook
	# does not reliably, see 001_memory.pl), and sending the second is
	# what makes the ClientRead wait *between* them -- now that a
	# payload exists to record into -- complete and show up as a row.
	$B->query_safe("SET pg_wait_event_tracing.capture = stats;");
	$B->query_safe("SELECT 1;");
	$B->query_safe("SELECT 1;");

	my $b_procnumber = $node->safe_psql(
		'postgres',
		"SELECT procnumber FROM pg_stat_wait_event_timing "
		  . "WHERE pid = $b_pid LIMIT 1;");
	is($b_procnumber, $a_procnumber,
		"B's own procnumber column agrees with the ProcNumber the loop found"
	);

	# B has some row of its own fresh activity, but never a PgSleep row
	# -- it never called pg_sleep -- so a PgSleep row here could only be
	# A's leftover data.  Checked both as the superuser reader and by B
	# reading about itself via the function directly (the view is
	# revoked from PUBLIC, so the latter exercises the self-privilege
	# branch of the internal check rather than a granted view or
	# pg_read_all_stats membership).
	cmp_ok(
		$node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_stat_wait_event_timing WHERE pid = $b_pid;"
		),
		'>', 0,
		"superuser reader sees at least one row for B");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_stat_wait_event_timing "
			  . "WHERE pid = $b_pid AND wait_event = 'PgSleep';"
		),
		'0',
		"...but no PgSleep row, which would only be A's leftover data");
	cmp_ok(
		$B->query_safe(
			"SELECT count(*) FROM pg_stat_get_wait_event_timing(pg_backend_pid());"
		),
		'>', 0,
		"B itself sees at least one row via the function");
	is( $B->query_safe(
			"SELECT count(*) FROM pg_stat_get_wait_event_timing(pg_backend_pid()) "
			  . "WHERE wait_event = 'PgSleep';"
		),
		'0',
		"...and no PgSleep row there either, despite no view grant"
	);

	# The view itself stays off limits to a role with no
	# pg_read_all_stats, unlike the function form used above.  A
	# one-shot connection is used rather than B's own background_psql
	# session: BackgroundPsql starts psql with on_error_stop => 1, so
	# the permission error would make psql exit, and the next call into
	# $B would die with "process ended prematurely".
	my ($ret, $out, $err) = $node->psql(
		'postgres',
		'SELECT * FROM pg_stat_wait_event_timing;',
		connstr => $node->connstr('postgres') . ' user=regress_b');
	isnt($ret, 0, "B cannot read the view directly");
	like($err, qr/permission denied/,
		"...only the function about itself, as shown above");

	$B->quit;
}

done_testing();
