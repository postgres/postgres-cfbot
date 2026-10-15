# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: reset race across ProcNumber reuse (fix 5).
#
# A cross-backend reset is published as a generation bump under the
# control lock, but the pid/start-timestamp used to resolve the target
# were captured earlier, outside that lock.  If a successor reuses the
# target's ProcNumber in the window between resolution and taking the
# lock, the request must not land on the successor: pwet_request_reset()
# re-checks owner_pid/owner_start against the resolved target under the
# same lock that publishes the bump, so a mismatch (successor already
# attached) leaves the slot alone.
#
# This test forces exactly that window open with the
# "pg-wait-event-tracing-reset-before-publish" injection point (placed
# between resolution and taking the lock -- see pg_wait_event_tracing.c),
# swaps in a successor while the requester is parked there, and checks
# the successor's own counters and reset_count come out untouched.
#
# PGPROC's free list is FIFO, not LIFO: InitProcess() pops the head
# (src/backend/storage/lmgr/proc.c) and ProcKill() pushes to the tail,
# so a freed ProcNumber is only handed out again once every other free
# slot has been used first.  max_connections is kept small here so that
# "every other free slot" is a short list, and the successor is found
# by opening candidate connections in a loop, while the requester is
# still parked, until one lands on the target's ProcNumber or a
# generous, bounded number of attempts is exhausted.
#
# The final PgSleep count below is asserted as >= 3, not = 3: pg_sleep()
# loops, calling WaitLatch again until its own clock says the requested
# time is up, and on some platforms (seen on Windows in CI) the latch
# timeout and that clock can disagree, so a single call can record more
# than one wait. The module is right to count every one of them; what
# this test actually needs decided is reset_count, which stays an exact
# 0 either way.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $max_connections = 10;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_wait_event_tracing, injection_points'");
$node->append_conf('postgresql.conf', "max_connections = $max_connections");
# Keep pg_sleep() running in the session that issued it, not a parallel
# worker, so its wait is recorded under the pid this test is watching.
$node->append_conf('postgresql.conf', "debug_parallel_query = off");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

my $point = 'pg-wait-event-tracing-reset-before-publish';

# A: one recorded wait, then note its pid and ProcNumber.
my $A = $node->background_psql('postgres');
$A->query_safe("SET pg_wait_event_tracing.capture = stats;");
$A->query_safe("SELECT pg_sleep(0.01);");
my $a_pid = $A->query_safe("SELECT pg_backend_pid();");
my $a_procnumber = $node->safe_psql(
	'postgres',
	"SELECT procnumber FROM pg_stat_wait_event_timing "
	  . "WHERE pid = $a_pid AND wait_event = 'PgSleep';");

# R: a superuser session that attaches the injection point and then
# starts a reset of A's session, which will block right before
# publishing the request.
my $R = $node->background_psql('postgres');
$R->query_safe("SELECT injection_points_attach('$point', 'wait');");
$R->query_until(
	qr/reset_launched/,
	"\\echo reset_launched\n"
	  . "SELECT pg_stat_reset_wait_event_timing($a_pid);\n");

$node->wait_for_event('client backend', $point);

# While R is parked at the injection point, replace A with B.
$A->quit;
$node->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $a_pid);"
) or die "backend $a_pid did not disappear from pg_stat_activity";

# Open candidate connections, one at a time, until one lands on A's
# ProcNumber -- pg_stat_get_backend_idset()'s id is the same
# proc_number the module's own "procnumber" column reports -- or the
# budget below is exhausted.  Each candidate runs only this query.
my $B;
my $attempts = 0;
my $max_attempts = 3 * $max_connections;
while ($attempts < $max_attempts)
{
	$attempts++;
	my $candidate = $node->background_psql('postgres');
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

my $b_pid;
if (defined $B)
{
	# Attach with a fresh owner token while R is still parked, so that
	# when R's stale request does reach the lock below, it finds this
	# ProcNumber already reassigned rather than merely unowned.
	$B->query_safe("SET pg_wait_event_tracing.capture = stats;");
	$B->query_safe("SELECT pg_sleep(0.01);");
	$B->query_safe("SELECT pg_sleep(0.01);");
	$b_pid = $B->query_safe("SELECT pg_backend_pid();");
}

# Now let R's stale request through, regardless of whether B was found
# above: R must not be left blocked at the injection point through the
# rest of the test (or its teardown).  It targeted A's old owner
# token, which -- if B attached above -- has since been overwritten,
# so it must not touch B's slot.
$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point');");
$R->quit;

SKIP:
{
	skip "ProcNumber $a_procnumber was not reused by any of $attempts "
	  . "connections; cannot exercise the race in this run", 2
	  unless defined $B;

	# One more wait after the release, so a wrongly-applied reset (which
	# would only be noticed at the *next* wait_end -- see
	# t/003_reset_acl.pl) has every opportunity to show up here too.
	$B->query_safe("SELECT pg_sleep(0.01);");

	cmp_ok(
		$node->safe_psql(
			'postgres',
			"SELECT calls FROM pg_stat_wait_event_timing "
			  . "WHERE pid = $b_pid AND wait_event = 'PgSleep';"
		),
		'>=', 3,
		"B's PgSleep count reflects all three of its own waits");
	is( $node->safe_psql(
			'postgres',
			"SELECT reset_count FROM pg_stat_wait_event_timing_overflow "
			  . "WHERE pid = $b_pid;"
		),
		'0',
		"the reset aimed at A's stale token was not consumed by B");

	$B->quit;
}

$node->safe_psql('postgres', "SELECT injection_points_detach('$point');");

done_testing();
