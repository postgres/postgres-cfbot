
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX-native overflow write-path buffer-lock livelock, reproduced through
# ordinary SQL with many concurrent sessions.
#
# The bug (seen first by HammerDB TPROC-C, 50 WH / 64 VU): under concurrent
# wide writes every backend wedges forever in LockBufferInternal with no
# deadlock report, because the deadlock is at the LWLock (buffer content lock)
# level, where there is no detector.  Two stack shapes share the stall:
#
#   flux_tuple_insert -> LockBufferInternal
#   flux_tuple_update -> FluxFormTupleForceShrink -> FluxStoreOverflowColumn
#       -> flux_overflow_add_chunk -> LockBufferInternal
#
# The "collected" overflow write path (flux_overflow_add_chunk) picks chunk
# pages in FSM order with FluxGetPageWithFreeSpace and HOLDS each one
# EXCLUSIVE-locked in FluxOverflowBuffers until the caller emits a single WAL
# record for the tuple op plus all its chunks.  Those chunk pages are ordinary
# FLUX data pages that plain inserts and other values' chunks also lock, in
# their own FSM order.  Several page locks held at once, acquired in arbitrary
# order by many backends, is a classic lock-order inversion with no deadlock
# detection -> permanent livelock.
#
# An in-place UPDATE reaches the collected path through force-shrink: when a
# row grown on a full page cannot fit, FluxFormTupleForceShrink pushes every
# varlena wider than a locator off-page.  Plain wide INSERTs reach the same
# collected path directly (several chunks per value).
#
# This test drives 48 concurrent pgbench sessions doing exactly that mix
# against a packed FLUX table, in the background, while the main process polls
# a progress counter.  If progress stops for the stall window it dumps the
# per-backend wait events, FAILS, and stops the cluster immediately so the
# suite never hangs.  It has a hard runtime cap well under three minutes.
#
# It is RED on the unfixed tree.  It stays RED until the overflow write path
# stops holding multiple chunk-page locks across the FSM-ordered allocation of
# the next chunk.  Any fix that removes the multi-lock hold makes it GREEN; it
# adds no injection points and drives only ordinary SQL, so it is valid for
# any fix approach.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start);
use Time::HiRes qw(time sleep);

# ---------------------------------------------------------------------------
# Parameters (calibrated to be RED in 5/5 runs under the cassert debug build).
# ---------------------------------------------------------------------------
my $CLIENTS       = 48;     # concurrent sessions (brief asks 32-64)
my $THREADS       = 8;      # pgbench worker threads
my $DRIVE_SECONDS = 75;     # how long pgbench is asked to run
my $PRELOAD_ROWS  = 20000;  # packed customer rows so pages are full
my $WIDE_BYTES    = 6000;   # incompressible width -> several overflow chunks
my $STALL_SECONDS = 30;     # no progress for this long == livelock
my $POLL_SECONDS  = 2;      # progress poll cadence
my $HARD_CAP      = 150;    # absolute runtime ceiling (< 3 min)

# Diagnostic knobs (env), so the fixer can probe which paths matter without
# editing the test.  Defaults reproduce the TPROC-C mix.
#   FLUX_LLREPRO_FILLFACTOR : table fillfactor (default 100)
#   FLUX_LLREPRO_MODE       : 'mix' (default) or 'insert' (INSERT-only, no
#                             force-shrink) -- answers whether the hang needs
#                             the UPDATE force-shrink path at all.
my $FILLFACTOR = $ENV{FLUX_LLREPRO_FILLFACTOR} || 100;
my $MODE       = $ENV{FLUX_LLREPRO_MODE} || 'mix';

# ---------------------------------------------------------------------------
# Cluster.  cassert debug build; keep autovacuum from moving pages under us,
# and give enough connection slots for the driver plus the monitor.
# ---------------------------------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('flux_overflow_livelock');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
max_connections = 100
fsync = off
deadlock_timeout = 1s
log_lock_waits = on
});
$node->start;

my $DB = 'postgres';

# An incompressible, caller-sized text literal: md5 hex is ~random, repeated.
# md5()::text is 32 chars; $WIDE_REPS repeats give exactly $WIDE_WIDTH bytes.
my $WIDE_REPS  = int(($WIDE_BYTES + 31) / 32);
my $WIDE_WIDTH = $WIDE_REPS * 32;

sub wide_literal
{
	my ($seed) = @_;
	return "repeat(md5($seed), $WIDE_REPS)";
}

# ---------------------------------------------------------------------------
# Schema: a TPC-C-customer-shaped FLUX table, fillfactor $FILLFACTOR, packed.
# narrow columns are the static identity; data/extra are what writers grow.
# All wide columns are STORAGE EXTERNAL so they take the FLUX-native overflow
# path (chunks on FLUX data pages), not inline compression.
# A progress counter sequence proves liveness independent of any one session.
# ---------------------------------------------------------------------------
$node->safe_psql(
	$DB, qq{
CREATE SEQUENCE progress;
CREATE TABLE cust (
    id       int PRIMARY KEY,
    bal      numeric(12,2) NOT NULL,
    first    varchar(16) NOT NULL,
    last     varchar(16) NOT NULL,
    street   varchar(20) NOT NULL,
    phone    varchar(16) NOT NULL,
    data     varchar(8000) NOT NULL,
    extra    varchar(8000) NOT NULL
) USING flux WITH (fillfactor = $FILLFACTOR);
ALTER TABLE cust ALTER COLUMN data  SET STORAGE EXTERNAL;
ALTER TABLE cust ALTER COLUMN extra SET STORAGE EXTERNAL;

-- Pack pages: short data so many rows per page and every page is full.
INSERT INTO cust
SELECT g, 10.00,
       left(md5(g::text), 16), left(md5((g+1)::text), 16),
       left(md5((g+2)::text), 20), left(md5((g+3)::text), 16),
       left(md5((g+4)::text), 40), left(md5((g+5)::text), 40)
FROM generate_series(1, $PRELOAD_ROWS) g;
});

my $maxid =
  $node->safe_psql($DB, 'SELECT max(id) FROM cust');

# ---------------------------------------------------------------------------
# pgbench scripts.
#
# upd: grow a random existing row's wide column to an incompressible value on
# a (full) page.  The in-place fit fails -> force-shrink -> collected overflow
# path, holding several chunk-page locks.
#
# ins: insert a brand-new wide row.  Several chunks per value go through the
# same collected path onto FSM-chosen (shared) FLUX pages.
#
# Each transaction bumps the progress sequence so the monitor can see liveness.
# ---------------------------------------------------------------------------
my $upd_script = $PostgreSQL::Test::Utils::tmp_check . '/164_upd.sql';
my $ins_script = $PostgreSQL::Test::Utils::tmp_check . '/164_ins.sql';

append_to_file(
	$upd_script, qq{
\\set rid random(1, $maxid)
BEGIN;
UPDATE cust
   SET data  = @{[ wide_literal(':rid * 100 + 1 || random()::text') ]},
       extra = @{[ wide_literal(':rid * 100 + 2 || random()::text') ]},
       bal   = bal + 1
 WHERE id = :rid;
SELECT nextval('progress');
COMMIT;
});

append_to_file(
	$ins_script, qq{
\\set nid random($maxid + 1, $maxid + 100000000)
BEGIN;
INSERT INTO cust VALUES (
    :nid, 0.00, 'ins', 'ins', 'ins', 'ins',
    @{[ wide_literal(':nid || random()::text') ]},
    @{[ wide_literal(':nid + 1 || random()::text') ]});
SELECT nextval('progress');
COMMIT;
});

# ---------------------------------------------------------------------------
# Launch pgbench in the BACKGROUND (so a wedge cannot hang this process), and
# poll progress.  Mix 60% force-shrink UPDATEs with 40% wide INSERTs.
# ---------------------------------------------------------------------------
my @pgbench_files =
  $MODE eq 'insert'
  ? ('--file' => "$ins_script\@1")           # INSERT-only: no force-shrink
  : ('--file' => "$upd_script\@6", '--file' => "$ins_script\@4");

my ($pg_out, $pg_err) = ('', '');
my $h = start(
	[
		'pgbench',
		'--no-vacuum',
		'--client'  => $CLIENTS,
		'--jobs'    => $THREADS,
		'--time'    => $DRIVE_SECONDS,
		@pgbench_files,
		$node->connstr($DB),
	],
	'>' => \$pg_out,
	'2>' => \$pg_err);

note "pgbench started: $CLIENTS clients, ${DRIVE_SECONDS}s, mode=$MODE, "
  . "fillfactor=$FILLFACTOR, wide=${WIDE_WIDTH}B, preload=${PRELOAD_ROWS} rows";

# Is pgbench still running?  pump_nb drains output non-blockingly and throws
# (or returns false) once the child has exited and its pipes reach EOF.  During
# a livelock every client is blocked inside the server, so pgbench emits
# nothing and never exits -> this stays true while the counter stays frozen.
sub driver_running
{
	return eval { $h->pump_nb; 1 } ? 1 : 0;
}

sub progress_count
{
	# last_value of the sequence == number of committed driver transactions.
	my $v = $node->safe_psql($DB, 'SELECT last_value FROM progress');
	return defined($v) && $v ne '' ? $v + 0 : 0;
}

sub dump_wait_events
{
	my ($why) = @_;
	my $dump = $node->safe_psql(
		$DB, q{
SELECT count(*) AS n, wait_event_type, wait_event
FROM pg_stat_activity
WHERE datname = current_database() AND pid <> pg_backend_pid()
      AND state = 'active'
GROUP BY wait_event_type, wait_event
ORDER BY n DESC});
	diag("=== $why: pg_stat_activity wait events ===\n$dump");
	my $det = $node->safe_psql(
		$DB, q{
SELECT pid, state, wait_event_type, wait_event,
       left(query, 40) AS query
FROM pg_stat_activity
WHERE datname = current_database() AND pid <> pg_backend_pid()
      AND state = 'active'
ORDER BY pid});
	diag("=== $why: per-backend detail ===\n$det");
	return $dump;
}

my $start   = time();
my $last    = progress_count();
my $last_at = $start;
my $stalled = 0;
my $stall_dump;

while (1)
{
	sleep($POLL_SECONDS);
	my $now  = time();
	my $seen = progress_count();

	if ($seen > $last)
	{
		$last    = $seen;
		$last_at = $now;
	}

	# Livelock: committed-transaction counter frozen for the stall window
	# while the driver is still supposedly running.
	if ($now - $last_at >= $STALL_SECONDS && driver_running())
	{
		$stalled    = 1;
		$stall_dump = dump_wait_events(
			"progress frozen at $last for ${STALL_SECONDS}s");
		last;
	}

	# Hard runtime ceiling, independent of pgbench's own clock.
	if ($now - $start >= $HARD_CAP)
	{
		$stalled    = 1;
		$stall_dump = dump_wait_events("hard cap ${HARD_CAP}s reached");
		last;
	}

	# Driver finished on its own with progress advancing -> no livelock.
	last unless driver_running();
}

# ---------------------------------------------------------------------------
# Tear down the driver and cluster FAST so the suite cannot hang, whatever we
# decided.  immediate stop kills wedged backends holding buffer locks.
# ---------------------------------------------------------------------------
if ($stalled)
{
	# Kill the whole pgbench process group; wedged clients will not exit.
	eval { $h->kill_kill(grace => 1); };
	eval { $h->finish; };
	$node->stop('immediate');
}
else
{
	eval { $h->finish; };
}

my $elapsed = sprintf('%.1f', time() - $start);
note "driver phase elapsed ${elapsed}s (hard cap ${HARD_CAP}s)";

# ---------------------------------------------------------------------------
# The assertion: writers must keep making progress.  On the unfixed tree the
# counter freezes while every backend sits in Buffer / BufferExclusive, so
# this FAILS (RED).  A fix that stops holding multiple chunk-page locks keeps
# the counter advancing and this PASSES.
# ---------------------------------------------------------------------------
ok(!$stalled,
	'concurrent overflow writers kept making progress (no buffer-lock livelock)'
) or diag("overflow writers stopped making progress:\n$stall_dump");

# ---------------------------------------------------------------------------
# Content check on the GREEN path only: the cluster is still up, so verify
# every surviving wide value reads back at its stored width (overflow fetch is
# correct, not just unblocked).  Skipped when we had to immediate-stop.
# ---------------------------------------------------------------------------
SKIP:
{
	skip 'cluster was stopped after a livelock', 1 if $stalled;

	# Wide rows are the INSERTed rows and any UPDATEd preload row.  Every
	# non-narrow value must read back at the exact overflow width; a short or
	# long one would mean a corrupt / truncated overflow fetch.
	my $bad = $node->safe_psql(
		$DB, qq{
SELECT count(*) FROM cust
WHERE length(data)  > 100 AND length(data)  <> $WIDE_WIDTH
   OR length(extra) > 100 AND length(extra) <> $WIDE_WIDTH});
	is($bad, '0', 'every wide value reads back at its written width');
}

done_testing();
