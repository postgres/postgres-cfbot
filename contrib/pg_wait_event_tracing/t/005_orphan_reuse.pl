# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: trace ring orphan lifecycle and reclaim (fix 3).
#
# On exit, a backend's trace ring is not freed: trace_state moves to
# ORPHANED and trace_owner_pid/trace_owner_start (independent of the
# stats-level owner_pid/owner_start -- see the PwetSlot comment in
# pg_wait_event_tracing.c) are left untouched, so pg_get_wait_event_trace()
# keeps attributing the ring to its producer post-mortem, like a flight
# recorder.  A successor that later attaches trace on the same ProcNumber
# reclaims (frees) the orphan as a side effect of its own attach, with no
# separate step and no call to the administrative sweep function.  This
# test exercises both reclaim paths: automatic (a live successor reusing
# the ProcNumber) and explicit (pg_stat_clear_orphaned_wait_event_rings(),
# for a ProcNumber nobody ever reuses).
#
# PGPROC's free list is FIFO, not LIFO: InitProcess() pops the head
# (src/backend/storage/lmgr/proc.c) and ProcKill() pushes to the tail, so
# a freed ProcNumber is only handed out again once every other free slot
# has been used first.  max_connections is kept small here so that "every
# other free slot" is a short list, and a successor is found by opening
# candidate connections in a loop, each checking its own ProcNumber via
# pg_stat_get_backend_idset()/pg_stat_get_backend_pid() (the same
# ProcNumber this module's own "procnumber" column reports) and dropping
# itself if it isn't the one being waited for.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $max_connections = 10;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(auth_extra => ['--create-role', 'regress_orphan']);
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_wait_event_tracing'");
$node->append_conf('postgresql.conf', "max_connections = $max_connections");
# Keep pg_sleep() running in the session that issued it, not a parallel
# worker, so its wait is recorded under the pid/ring this test is
# watching.
$node->append_conf('postgresql.conf', "debug_parallel_query = off");
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE ROLE regress_orphan LOGIN;
));

# ---------------------------------------------------------------------
# Part 1: A traces and exits; its ring stays readable post-mortem; a
# successor B that reuses A's ProcNumber gets a fresh ring, reclaiming
# (without ever calling the sweep function) A's orphan in the process.
# ---------------------------------------------------------------------
my $A = $node->background_psql('postgres');
$A->query_safe("SET pg_wait_event_tracing.capture = trace;");
$A->query_safe("SELECT pg_sleep(0.01);");

my $a_pid = $A->query_safe("SELECT pg_backend_pid();");
my $a_procnumber = $node->safe_psql(
	'postgres',
	"SELECT procnumber FROM pg_stat_wait_event_timing "
	  . "WHERE pid = $a_pid AND wait_event = 'PgSleep';");
# The exact PgSleep record, read from A's own session, so its survival
# (not just "some PgSleep row") can be confirmed post-mortem below.
my $a_pgsleep_ts = $A->query_safe(
	"SELECT timestamp_ns FROM pg_backend_wait_event_trace "
	  . "WHERE wait_event = 'PgSleep' ORDER BY seq DESC LIMIT 1;");

$A->quit;
$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $a_pid);"
) or die "backend $a_pid did not disappear from pg_stat_activity";

is( $node->safe_psql(
		'postgres',
		"SELECT owner_pid FROM pg_get_wait_event_trace($a_procnumber) "
		  . "WHERE wait_event = 'PgSleep' ORDER BY seq DESC LIMIT 1;"
	),
	$a_pid,
	"A's orphaned ring is still readable post-mortem, still tagged with A's pid"
);
is( $node->safe_psql(
		'postgres',
		"SELECT timestamp_ns FROM pg_get_wait_event_trace($a_procnumber) "
		  . "WHERE wait_event = 'PgSleep' ORDER BY seq DESC LIMIT 1;"
	),
	$a_pgsleep_ts,
	"...and the exact PgSleep record survives, untouched, in the orphan");

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

SKIP:
{
	skip "ProcNumber $a_procnumber was not reused by any of $attempts "
	  . "connections; cannot exercise the reclaim path in this run", 3
	  unless defined $B;

	my $b_pid = $B->query_safe("SELECT pg_backend_pid();");

	# B attaches trace and records its own wait; this alone, with no call
	# to pg_stat_clear_orphaned_wait_event_rings(), must reclaim A's
	# orphan (pwet_attach_trace() frees any pre-existing ring at the
	# slot before publishing its own).
	$B->query_safe("SET pg_wait_event_tracing.capture = trace;");
	$B->query_safe("SELECT pg_sleep(0.02);");

	is( $node->safe_psql(
			'postgres',
			"SELECT owner_pid FROM pg_get_wait_event_trace($a_procnumber) "
			  . "WHERE wait_event = 'PgSleep' ORDER BY seq DESC LIMIT 1;"
		),
		$b_pid,
		"B owns a fresh ring at the reused ProcNumber");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_get_wait_event_trace($a_procnumber) "
			  . "WHERE timestamp_ns = $a_pgsleep_ts;"
		),
		'0',
		"A's record is gone from the reclaimed ring");
	cmp_ok(
		$node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_get_wait_event_trace($a_procnumber) "
			  . "WHERE wait_event = 'PgSleep';"
		),
		'>',
		0,
		"...replaced by B's own PgSleep record(s)");

	$B->quit;
}

# ---------------------------------------------------------------------
# Part 2: A2 traces and exits; nobody reuses its ProcNumber.  The
# explicit sweep function frees the orphan; a non-superuser cannot call
# it at all.
# ---------------------------------------------------------------------
my $A2 = $node->background_psql('postgres');
$A2->query_safe("SET pg_wait_event_tracing.capture = trace;");
$A2->query_safe("SELECT pg_sleep(0.01);");
my $a2_pid = $A2->query_safe("SELECT pg_backend_pid();");
my $a2_procnumber = $node->safe_psql(
	'postgres',
	"SELECT procnumber FROM pg_stat_wait_event_timing "
	  . "WHERE pid = $a2_pid AND wait_event = 'PgSleep';");
$A2->quit;
$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $a2_pid);"
) or die "backend $a2_pid did not disappear from pg_stat_activity";

cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($a2_procnumber);"),
	'>',
	0,
	"A2's orphaned ring is readable before the sweep");

# A non-superuser cannot sweep orphaned rings.  A one-shot connection is
# required here, not a BackgroundPsql session: the expected ERROR would
# make a plain background_psql session (on_error_stop by default) die.
my ($ret, $out, $err) = $node->psql(
	'postgres',
	'SELECT pg_stat_clear_orphaned_wait_event_rings();',
	connstr => $node->connstr('postgres') . ' user=regress_orphan');
isnt($ret, 0, "a non-superuser cannot sweep orphaned trace rings");
like($err, qr/permission denied/, "...and gets a permission-denied error");

my $freed = $node->safe_psql('postgres',
	'SELECT pg_stat_clear_orphaned_wait_event_rings();');
cmp_ok($freed, '>=', 1, "the superuser sweep frees at least A2's orphan");

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($a2_procnumber);"),
	'0',
	"A2's orphan is gone after the sweep");

$node->stop;

done_testing();
