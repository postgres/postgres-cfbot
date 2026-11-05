# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: lazy, per-process wait-hook installation.  A
# process installs its wait_event_begin_hook/
# wait_event_end_hook the first time pg_wait_event_tracing.capture becomes
# non-off in that process, and never removes them again; a process that
# never enables capture never installs them at all.
#
# This node starts with capture off, so nothing installs its hooks at
# postmaster start (unlike t/006_server_processes.pl's node1).  Session A
# enables stats and records a wait; session B never touches capture.  B's
# pg_wait_event_tracing_hooks_installed() must be false, and B must have
# no rows in pg_stat_wait_event_timing, even though A does.  A subsequent
# reload that turns capture on cluster-wide then makes B install its own
# hooks too, at its next safe point -- exactly the "capture set by reload"
# scenario (d) from the pg_wait_event_tracing.c commit that added this
# lazy installation.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');

# Two long-lived sessions, so each one's own hook-installation state (a
# process-local flag) can be probed on that same backend before and after
# the reload below.
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');

my $a_pid = $a->query_safe('SELECT pg_backend_pid();');
my $b_pid = $b->query_safe('SELECT pg_backend_pid();');

# Session A enables stats and records a deterministic wait.
$a->query_safe('SET pg_wait_event_tracing.capture = stats;');
$a->query_safe('SELECT pg_sleep(0.01);');

is($a->query_safe('SELECT pg_wait_event_tracing_hooks_installed();'),
	't', 'session A has installed its wait hooks after enabling stats');

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_wait_event_timing WHERE pid = $a_pid;"
	),
	't',
	'session A has rows in pg_stat_wait_event_timing');

# Session B never enabled capture, so it never installed its hooks, and it
# has no rows of its own.
is($b->query_safe('SELECT pg_wait_event_tracing_hooks_installed();'),
	'f', 'session B has not installed its wait hooks');

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_wait_event_timing WHERE pid = $b_pid;"
	),
	'0',
	'session B has no rows in pg_stat_wait_event_timing');

# A cluster-wide reload turning capture on: every process, including B,
# installs from its own assign hook at its next safe point (scenario (d)
# in the pg_wait_event_tracing.c commit comment).
$node->safe_psql(
	'postgres', q(
	ALTER SYSTEM SET pg_wait_event_tracing.capture = 'stats';
	SELECT pg_reload_conf();
));

# pg_reload_conf() only asks the postmaster to signal every backend with
# SIGHUP; it does not wait for any of them to actually act on it.  Each
# backend, including session B's, only re-reads its configuration (and so
# only runs pwet_assign_capture() again) the next time it checks for
# interrupts -- in practice, the next command it processes -- which can
# be an arbitrarily short but non-zero time after this call returns.  A
# single immediate query_safe() can therefore observe the pre-reload 'f'
# on a slow or loaded runner even though B is about to install its hooks;
# poll instead, the same bounded way Cluster.pm's poll_query_until() does
# (up to $PostgreSQL::Test::Utils::timeout_default seconds, sleeping
# 0.1s between attempts), but running the query on B's own background
# session rather than a fresh connection, since the installed-hooks flag
# is process-local to B.
my $b_installed = 'f';
for (my $attempts = 0;
	$attempts < 10 * $PostgreSQL::Test::Utils::timeout_default;
	$attempts++)
{
	$b_installed =
	  $b->query_safe('SELECT pg_wait_event_tracing_hooks_installed();');
	last if $b_installed eq 't';
	usleep(100_000);
}
ok( $b_installed eq 't',
	'session B installs its wait hooks once capture is turned on by a reload'
);

$a->quit;
$b->quit;
$node->stop;

done_testing();
