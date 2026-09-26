# Copyright (c) 2026, PostgreSQL Global Development Group

# Server-side processes (the checkpointer, background writer, WAL writer,
# I/O workers, and -- during recovery -- the startup process) never parse
# a query or run the executor, so they cannot attach through the same
# entry points a client backend uses.  Plan section 4.2a's "option A":
# when pg_wait_event_tracing.capture is already non-off in the
# configuration at postmaster start, the module additionally reserves a
# fixed-size region with one slot per possible server-side ProcNumber, and
# each such process claims its own slot the first time it waits on
# anything -- so these processes collect from process start, with no
# configuration reload ever required.
#
# This test exercises that reserved-region path (node1, cases 1-3) and its
# fallback for a node that starts with capture off, where server-side
# processes instead pick up capture at the next reload the same way a
# client backend would on its next statement (node2, case 4) -- the
# scenario that depends on pwet_assign_capture() using the *incoming*
# capture value, not the not-yet-stored GUC variable, to decide whether it
# is safe to attach synchronously (see pwet_capture_effective in
# pg_wait_event_tracing.c).

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# ---------------------------------------------------------------------
# node1: capture = stats already in postgresql.conf at postmaster start.
# ---------------------------------------------------------------------
my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(allows_streaming => 1);
$node1->append_conf(
	'postgresql.conf', qq(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.capture = 'stats'
debug_parallel_query = off
));
$node1->start;

$node1->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');

# A little write activity, plus an explicit checkpoint, gives the
# checkpointer, background writer and WAL writer something to do promptly
# rather than relying on their default multi-second/multi-minute idle
# cycles (checkpoint_timeout defaults to 5 minutes).  I/O workers need no
# such nudge: io_min_workers keeps at least two of them alive from server
# start, idling in their own main loop (wait event IO_WORKER_MAIN) even
# with no read/write demand at all.
$node1->safe_psql(
	'postgres', q(
	CREATE TABLE wet_activity AS
		SELECT i, repeat('x', 100) AS pad FROM generate_series(1, 10000) i;
	CHECKPOINT;
));

# Case 1: at least one of checkpointer/walwriter/background writer has
# rows in pg_stat_wait_event_timing, despite this node never having
# reloaded its configuration.  This test's subject is "a server-side
# process collects without a reload", not "every one of these three
# background processes performs a recorded wait within a fixed timeout on
# a possibly slow, idle CI runner" -- polling for each individually (an
# earlier version of this test did) is not reliable there: CI run
# 34703751075 timed out on the checkpointer check on MinGW while
# walwriter, background writer, and the I/O worker check below all
# passed within 0.1s immediately afterward (both region checks passed
# too), and the same thing happened to walwriter instead on MSVC, with
# checkpointer passing -- a different single process missing each run,
# everything else green.  The module was working correctly both times.
#
# None of the fixed literal backend-type values below need SQL-escaping.
my @server_backend_types = ('checkpointer', 'walwriter', 'background writer');
my $backend_type_list = join(', ', map { "'$_'" } @server_backend_types);

ok( $node1->poll_query_until(
		'postgres',
		"SELECT EXISTS (SELECT 1 FROM pg_stat_wait_event_timing WHERE backend_type IN ($backend_type_list))"
	),
	'at least one of checkpointer/walwriter/background writer has rows in pg_stat_wait_event_timing without a reload'
);

# Soft, non-polling evidence for each type individually: assert only for
# whichever ones already have rows by now (the disjunction above already
# proved the reserved-region path works at all), and skip -- not fail --
# the rest, since a specific one's own wait may simply not have landed
# yet on a slow runner.
for my $backend_type (@server_backend_types)
{
	my $has_rows = $node1->safe_psql('postgres',
		"SELECT EXISTS (SELECT 1 FROM pg_stat_wait_event_timing WHERE backend_type = '$backend_type')"
	);

  SKIP:
	{
		skip "$backend_type has no rows yet on this run; the disjunction above already covers it",
		  1
		  unless $has_rows eq 't';

		ok(1, "$backend_type has rows in pg_stat_wait_event_timing without a reload");
	}
}

# I/O workers only exist under io_method = worker; several CI jobs force
# io_method = io_uring via PG_TEST_INITDB_EXTRA_OPTS, which has none, so
# this check would otherwise time out there instead of failing cleanly.
my $io_method = $node1->safe_psql('postgres', 'SHOW io_method');
SKIP:
{
	skip "io_method is '$io_method', not 'worker': no I/O workers exist", 1
	  unless $io_method eq 'worker';

	ok( $node1->poll_query_until(
			'postgres',
			q(SELECT EXISTS (SELECT 1 FROM pg_stat_wait_event_timing WHERE backend_type = 'io worker'))
		),
		'io worker has rows in pg_stat_wait_event_timing without a reload'
	);
}

# Case 2: pg_shmem_allocations shows the reserved region, at least as
# large as |R| slots at a conservative lower bound for the per-slot
# stride.  |R| = autovacuum_worker_slots + NUM_SPECIAL_WORKER_PROCS (2) +
# max_worker_processes + max_wal_senders + PWET_NON_IO_AUX_PROCS (6) +
# io_max_workers (plan section 4.2a; the "2" and "6" are proc.h constants,
# not GUCs, so they are literals here too).  200000 bytes/slot is
# comfortably below the ~206-212 KiB the C code actually computes at the
# default pg_wait_event_tracing.max_tranches (192) on every platform this
# has been checked on, without this test having to reproduce that
# platform-dependent struct-layout arithmetic itself.
my $num_server_slots = $node1->safe_psql(
	'postgres', q(
	SELECT current_setting('autovacuum_worker_slots')::int
		 + 2
		 + current_setting('max_worker_processes')::int
		 + current_setting('max_wal_senders')::int
		 + 6
		 + current_setting('io_max_workers')::int
));

my $region_row = $node1->safe_psql(
	'postgres', q(
	SELECT size FROM pg_shmem_allocations
	WHERE name = 'pg_wait_event_tracing server processes'
));

ok(length($region_row), 'server-process region is present in pg_shmem_allocations');
cmp_ok($region_row, '>=', $num_server_slots * 200000,
	'server-process region is at least |R| slots wide');

# Case 3: a standby created from a base backup of node1 (same
# configuration, capture already on) shows startup-process recovery waits
# without any reload on the standby either.
my $backup_name = 'node1_backup';
$node1->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node1, $backup_name, has_streaming => 1);
$node_standby->start;

# Give the standby's startup process WAL to keep applying/waiting on.
$node1->safe_psql(
	'postgres', q(
	INSERT INTO wet_activity SELECT i, repeat('y', 100) FROM generate_series(1, 10000) i;
));
$node1->wait_for_replay_catchup($node_standby);

# A plain poll_query_until() here can time out unconditionally, no matter
# how long it waits: with deferred accounting, a completed wait's counters
# are written out only at the backend's own *next* wait_start, and once
# the standby is caught up its only further wait,
# WaitForWALToBecomeAvailable()'s streaming-source wait, has no timeout at
# all -- so with nothing later to trigger a flush, the one-shot INSERT
# above can leave the row never appearing at all.
#
# Keep sending small bursts of WAL from the primary while polling, so the
# startup process keeps re-entering that wait: each new wait's begin
# flushes the previous one, so the row appears within a couple of
# iterations regardless of runner speed.
my $standby_startup_has_rows = 0;
for (my $attempts = 0;
	$attempts < 10 * $PostgreSQL::Test::Utils::timeout_default;
	$attempts++)
{
	if ($node_standby->safe_psql(
			'postgres',
			q(SELECT EXISTS (SELECT 1 FROM pg_stat_wait_event_timing WHERE backend_type = 'startup'))
		) eq 't')
	{
		$standby_startup_has_rows = 1;
		last;
	}

	# Nudge the startup process into another wait/flush cycle.
	$node1->safe_psql('postgres',
		q(INSERT INTO wet_activity SELECT i FROM generate_series(1, 10) i));
	$node1->wait_for_replay_catchup($node_standby);
	usleep(100_000);
}

ok($standby_startup_has_rows,
	'standby startup process has rows in pg_stat_wait_event_timing without a reload'
);

# ---------------------------------------------------------------------
# node2: capture off at postmaster start -- no region is ever reserved,
# so server-side processes fall back to attaching via the DSA path at the
# next configuration reload, exactly like a client backend attaching on
# its next statement.
# ---------------------------------------------------------------------
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init;
$node2->append_conf(
	'postgresql.conf', qq(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
));
$node2->start;

$node2->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');

is( $node2->safe_psql(
		'postgres', q(
		SELECT count(*) FROM pg_shmem_allocations
		WHERE name = 'pg_wait_event_tracing server processes'
	)),
	'0',
	'no server-process region exists when capture starts off');

$node2->safe_psql(
	'postgres', q(
	ALTER SYSTEM SET pg_wait_event_tracing.capture = 'stats';
	SELECT pg_reload_conf();
));

$node2->safe_psql(
	'postgres', q(
	CREATE TABLE wet_activity2 AS SELECT i FROM generate_series(1, 1000) i;
	CHECKPOINT;
));

ok( $node2->poll_query_until(
		'postgres',
		q(SELECT EXISTS (SELECT 1 FROM pg_stat_wait_event_timing WHERE backend_type = 'checkpointer'))
	),
	'checkpointer has rows after capture is turned on by a reload'
);

$node_standby->stop;
$node1->stop;
$node2->stop;

done_testing();
