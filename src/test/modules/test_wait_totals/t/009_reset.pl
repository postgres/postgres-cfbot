# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: pg_stat_reset_wait_event_totals().
# A reset never modifies a ledger; it scans all of them and publishes the sum
# as a baseline that readers subtract, so it is a scan-based cut, not an
# instantaneous global one.  Capture is off cluster-wide and sessions turn it
# on themselves, so assertions are on the dedicated event only.
#
#  - totals of the dedicated event restart from zero, the generation
#    advances, the reset scan times appear;
#  - a completion that was made before the reset but not yet published when
#    the scan read its ledger is counted after the reset (exact, with the
#    emitter held in its CPU stretch);
#  - per-backend resets do not touch the totals, and the totals' reset does
#    not touch the per-backend view;
#  - resets concurrent with emitters and snapshot readers: no error, and
#    within one generation the counts never decrease;
#  - a diff across a reset is an error.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use PostgreSQL::Test::Cluster;
use TwtUtil;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
TwtUtil::harden($node);
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
autovacuum = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

sub calls
{
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'");
}

sub wait_calls
{
	my ($want, $what) = @_;
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $want FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "$what: calls never reached $want (now " . calls() . ')';
}

sub generation
{
	return $node->safe_psql('postgres',
		'SELECT reset_generation FROM pg_stat_wait_event_totals_info');
}

# ---------------------------------------------------------------------
# Basic reset.
# ---------------------------------------------------------------------
is($node->safe_psql('postgres',
		'SELECT stats_reset IS NULL AND reset_scan_start IS NULL
		 FROM pg_stat_wait_event_totals_info'),
	't', 'no reset yet: no reset times');
my $gen0 = generation();

my $w = $node->background_psql('postgres');
$w->query_safe('SET pg_wait_event_tracing.capture = stats');
my $wpid = $w->query_safe('SELECT pg_backend_pid()');
$w->query_safe('SELECT test_wait_totals_emit(5)');
wait_calls(5, 'before reset');

$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
is(generation(), $gen0 + 1, 'the generation advanced by one');
is(calls(), '0', 'the totals of the dedicated event restarted from zero');
is($node->safe_psql('postgres',
		'SELECT stats_reset IS NOT NULL AND reset_scan_start <= stats_reset
		 FROM pg_stat_wait_event_totals_info'),
	't', 'the reset scan times are published');

$w->query_safe('SELECT test_wait_totals_emit(3)');
wait_calls(3, 'after reset');
is(calls(), '3', 'counting continues from zero after the reset');

# ---------------------------------------------------------------------
# Scan semantics: a completion made before the reset but still pending when
# the scan reads its ledger is counted after the reset.
# ---------------------------------------------------------------------
$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
$w->query_until(qr/started/,
	"\\echo started\nSELECT test_wait_totals_emit_hold(4);\n");
$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
  or die 'the emitter never reached its CPU stretch';
is(calls(), '6', 'three of the four are published while the emitter holds');
$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
is(calls(), '0', 'reset while the emitter holds');
$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
$w->query_safe('SELECT 1');
wait_calls(1, 'pending completion after the reset');
is(calls(), '1',
	'the pending completion is counted after the reset (scan-based cut)');

# ---------------------------------------------------------------------
# Independence from the per-backend resets, both ways.
# ---------------------------------------------------------------------
my $legacy = sub {
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_timing
		 WHERE pid = $wpid AND wait_event = 'TestWaitTotals'");
};
$w->query_safe('SELECT test_wait_totals_emit(2)');
wait_calls(3, 'before the per-backend resets');
my $g = generation();
$w->query_safe('SELECT pg_stat_reset_wait_event_timing()');
$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_timing_all()');
$w->query_safe('SELECT 1');
is(calls(), '3', 'per-backend resets do not change the totals');
is(generation(), $g, 'per-backend resets do not change the generation');

$w->query_safe('SELECT test_wait_totals_emit(2)');
$w->query_safe('SELECT 1');
my $legacy_before = $legacy->();
$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
is($legacy->(), $legacy_before, 'the totals reset does not touch the per-backend view');
cmp_ok($legacy_before, '>', 0, 'and the per-backend view has something to keep');
$w->quit;

# ---------------------------------------------------------------------
# A controlled schedule across reset boundaries, with exact counts.  Every
# emission so far (5 + 3 + 4 + 2 + 2 = 16) and below is tracked in $prior;
# completions held pending at a reset are counted after it.
# ---------------------------------------------------------------------
my $prior = 16;
{
	my $s = $node->background_psql('postgres');
	$s->query_safe('SET pg_wait_event_tracing.capture = stats');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');

	$s->query_until(qr/started/, "\\echo started\nSELECT test_wait_totals_emit_hold(3);\n");
	$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
	  or die 'schedule 1: the emitter never reached its CPU stretch';
	$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
	is(calls(), '0', 'schedule: zero right after the first reset');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	$s->query_safe('SELECT 1');
	wait_calls(1, 'schedule: pending completion after the first reset');
	$prior += 3;

	$s->query_safe('SELECT test_wait_totals_emit(5)');
	$s->query_safe('SELECT 1');
	wait_calls(6, 'schedule: five more');
	$prior += 5;

	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
	$s->query_until(qr/started/, "\\echo started\nSELECT test_wait_totals_emit_hold(2);\n");
	$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
	  or die 'schedule 2: the emitter never reached its CPU stretch';
	is(calls(), '7', 'schedule: one of the two is published while it holds');
	$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
	is(calls(), '0', 'schedule: zero right after the second reset');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	$s->query_safe('SELECT 1');
	wait_calls(1, 'schedule: pending completion after the second reset');
	$prior += 2;
	$s->quit;
}

# Assert-build only: a reader suspended between its scan and its generation
# check while a reset publishes must notice it, start over and return the new
# generation; and the lifetime ledgers conserve everything emitted.
my $cassert = check_pg_config('#define USE_ASSERT_CHECKING 1');
if ($cassert)
{
	$node->safe_psql('postgres', q(
CREATE FUNCTION pwet_test_read_barrier_arm() RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_arm' LANGUAGE C;
CREATE FUNCTION pwet_test_read_barrier_waiting() RETURNS bool
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_waiting' LANGUAGE C;
CREATE FUNCTION pwet_test_read_barrier_release() RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_read_barrier_release' LANGUAGE C;
CREATE FUNCTION pwet_test_read_retries() RETURNS bigint
  AS 'pg_wait_event_tracing', 'pwet_test_read_retries' LANGUAGE C;
CREATE FUNCTION pwet_test_totals_absolute(bigint, OUT absolute bigint, OUT baseline bigint)
  AS 'pg_wait_event_tracing', 'pwet_test_totals_absolute' LANGUAGE C;
));
	my $retries0 = $node->safe_psql('postgres', 'SELECT pwet_test_read_retries()');
	my $gen_before = generation();
	$node->safe_psql('postgres', 'SELECT pwet_test_read_barrier_arm()');
	my $r = $node->background_psql('postgres');
	$r->query_until(qr/reading/, q(\echo reading
CREATE TEMP TABLE sn AS SELECT pg_wait_event_snapshot() AS s;
));
	$node->poll_query_until('postgres', 'SELECT pwet_test_read_barrier_waiting()')
	  or die 'the reader never reached the barrier';
	$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
	$node->safe_psql('postgres', 'SELECT pwet_test_read_barrier_release()');
	$r->query_safe('SELECT 1');
	is($r->query_safe(q(SELECT s->>'reset_generation' FROM sn)), $gen_before + 1,
		'a reader suspended during a reset returns the new generation');
	is($node->safe_psql('postgres', 'SELECT pwet_test_read_retries()'), $retries0 + 1,
		'because it noticed the move and started over');
	$r->quit;
}

# ---------------------------------------------------------------------
# A diff across a reset is an error.
# ---------------------------------------------------------------------
{
	my $a = $node->safe_psql('postgres', 'SELECT pg_wait_event_snapshot()');
	$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_totals()');
	my $b = $node->safe_psql('postgres', 'SELECT pg_wait_event_snapshot()');
	my ($ret, $out, $err) = $node->psql('postgres',
		"SELECT * FROM pg_wait_event_diff(\$pwet\$$a\$pwet\$::jsonb, \$pwet\$$b\$pwet\$::jsonb)");
	isnt($ret, 0, 'diff across a reset fails');
	like($err, qr/reset between the snapshots/, 'with the reset message');
}

# ---------------------------------------------------------------------
# Resets concurrent with emitters and readers.  Writers run until told to
# stop; the resetter and the reader each wait for writer progress between
# their steps, so every step really happens while the writers run.
# ---------------------------------------------------------------------
my $resets = 30;
my $nsamples = 200;
$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
my $gen_start = generation();

my @writers;
for my $i (1 .. 2)
{
	my $s = $node->background_psql('postgres');
	$s->query_safe('SET pg_wait_event_tracing.capture = stats');
	$s->query_until(qr/started/,
		"\\echo started\nSELECT test_wait_totals_emit_until_stopped(1);\n");
	push @writers, $s;
}

my $resetter = $node->background_psql('postgres');
$resetter->query_safe("SELECT test_wait_totals_wait_writers(2)");
$resetter->query_until(qr/resetting/, qq(\\echo resetting
DO \$\$
DECLARE p bigint := 0;
BEGIN
	FOR i IN 1..$resets LOOP
		p := test_wait_totals_wait_progress(p);
		PERFORM pg_stat_reset_wait_event_totals();
	END LOOP;
END \$\$;
));

my $reader = $node->background_psql('postgres');
$reader->query_safe("SELECT test_wait_totals_wait_writers(2)");
$reader->query_safe(qq(
CREATE TEMP TABLE snap (i int, gen bigint, calls bigint);
DO \$\$
DECLARE
	p bigint := 0;
	s jsonb;
BEGIN
	FOR i IN 1..$nsamples LOOP
		p := test_wait_totals_wait_progress(p);
		s := pg_wait_event_snapshot();
		INSERT INTO snap
		SELECT i, (s->>'reset_generation')::bigint,
		       coalesce((SELECT (e->>'calls')::bigint
		                 FROM jsonb_array_elements(s->'events') e
		                 WHERE e->>'name' = 'TestWaitTotals'), 0);
	END LOOP;
END \$\$;
));
$resetter->query_safe('SELECT 1');    # joins the reset loop; fails on any ERROR

is($reader->query_safe('SELECT count(*) FROM snap'), $nsamples,
	'the reader took all its samples without an error');
is($reader->query_safe(
		'SELECT count(*) FROM (SELECT gen, calls,
		   lag(gen) OVER (ORDER BY i) AS pg, lag(calls) OVER (ORDER BY i) AS pc
		 FROM snap) x
		 WHERE gen < pg OR (gen = pg AND calls < pc)'),
	'0', 'generation never goes back and counts never decrease within a generation');
$reader->quit;

$node->safe_psql('postgres', 'SELECT test_wait_totals_stop()');
my $emitted = $prior;
foreach my $s (@writers)
{
	my ($n) = split /\n/, $s->query_safe('SELECT 1');
	$emitted += $n;
	$s->quit;
}
$resetter->quit;
is(generation(), $gen_start + $resets, "all $resets resets were applied");
cmp_ok(calls(), '<=', $emitted, 'the totals never exceed what was emitted');
if ($cassert)
{
	my $key = $node->safe_psql('postgres', 'SELECT test_wait_totals_event_key()');
	my ($abs, $base) = split /\|/, $node->safe_psql('postgres',
		"SELECT absolute, baseline FROM pwet_test_totals_absolute($key)");
	is($abs, $emitted, "conservation: the ledgers hold exactly what was emitted ($emitted)");
	is(calls(), $abs - $base, 'and the view shows the ledgers minus the last baseline');
}

$node->stop;
done_testing();
