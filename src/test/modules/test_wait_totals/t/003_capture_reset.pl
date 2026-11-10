# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing across capture-level changes and
# per-backend resets.  The totals of the dedicated event are a lifetime
# record: every completed wait made while capture was on is counted exactly
# once, whatever happens to the per-backend payload afterwards.
#
# 4. All six transitions between off, stats and trace, repeated, with
#    emissions in between: the total never decreases and equals the number of
#    emissions made while capture was on.  Emissions while off count
#    nothing; a completion still pending when capture is switched is flushed
#    by the assign hook and counted.
# 5. Own, remote and reset-all resets with a completion pending: the instance
#    total keeps every completion exactly once, while the per-backend view
#    resets as it always did.

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
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

my $TOTAL = "SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
			 WHERE wait_event = 'TestWaitTotals'";

sub calls
{
	return $node->safe_psql('postgres', $TOTAL);
}

# The producer's temporary capture change must keep the scope of the setting
# it found: a SET LOCAL stays local and the session value comes back.
is($node->safe_psql('postgres',
		q(SET pg_wait_event_tracing.capture = off;
		  BEGIN;
		  SET LOCAL pg_wait_event_tracing.capture = trace;
		  SELECT test_wait_totals_emit_with_capture('stats', ARRAY[]::int[], 0);
		  SHOW pg_wait_event_tracing.capture;
		  COMMIT;
		  SHOW pg_wait_event_tracing.capture;)) =~ s/^\n//mgr,
	"trace\noff",
	'emit_with_capture keeps a SET LOCAL local and the session value after COMMIT');

# ---------------------------------------------------------------------
# 4. Capture transitions.  One session reads its own totals, which flush its
#    own pending completion first, so the count is exact at every step.
# ---------------------------------------------------------------------
my $s = $node->background_psql('postgres');
sub mine { return $s->query_safe($TOTAL); }

my $expected = 0;
my $prev = 0;
my $mode = 'off';
my @steps = qw(stats trace off trace stats off stats trace off);
push @steps, @steps;    # the six directions, twice over

foreach my $next (@steps)
{
	$s->query_safe("SET pg_wait_event_tracing.capture = $next");
	my $got = mine();
	cmp_ok($got, '>=', $prev, "$mode -> $next: total did not decrease");
	$prev = $got;
	$mode = $next;

	$s->query_safe('SELECT test_wait_totals_emit(3)');
	$expected += 3 unless $mode eq 'off';
	$got = mine();
	is($got, $expected, "capture $mode: 3 emissions "
		  . ($mode eq 'off' ? 'not counted' : 'counted'));
	$prev = $got;
}

# A completion pending at the moment capture changes is flushed by the
# assign hook and counted, because the wait happened while capture was on.
# The helper emits and changes the setting before returning to SQL, so no
# ClientRead and no statement marker publishes the completion first.
foreach my $pair ([qw(stats off)], [qw(trace off)], [qw(stats trace)],
	[qw(trace stats)])
{
	my ($from, $to) = @$pair;
	$s->query_safe("SET pg_wait_event_tracing.capture = $from");
	$s->query_safe("SELECT test_wait_totals_emit_then_set_capture(3, '$to')");
	$expected += 3;
	is(mine(), $expected, "pending completion counted on $from -> $to");
}
$s->query_safe('SET pg_wait_event_tracing.capture = off');
$s->quit;

# ---------------------------------------------------------------------
# 5. Resets with a completion pending.
# ---------------------------------------------------------------------
my $legacy = sub {
	my ($pid) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_timing
		 WHERE pid = $pid AND wait_event = 'TestWaitTotals'");
};

# Own reset: the pending completion is flushed first, then wiped from the
# per-backend payload; the instance total keeps it.
my $base = calls();
my $r = $node->background_psql('postgres');
$r->query_safe('SET pg_wait_event_tracing.capture = stats');
my $rpid = $r->query_safe('SELECT pg_backend_pid()');
$r->query_safe(
	'SELECT test_wait_totals_emit(4) \; SELECT pg_stat_reset_wait_event_timing()');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = $base + 4 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'own reset: the pending completion was lost';
is(calls(), $base + 4, 'own reset: all 4 counted exactly once');
is($legacy->($rpid), '0', 'own reset: per-backend count was reset');
$r->query_safe('SELECT test_wait_totals_emit(2)');
$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = $base + 6 FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'own reset: later emissions not counted';
is($legacy->($rpid), '2', 'own reset: per-backend view counts again from zero');
$r->quit;

# Remote reset and reset-all, requested while the target holds a pending
# completion (it is burning CPU after its 4 emissions, 3 of them flushed).
# The reset takes effect at the target's next flush, before the pending
# completion is added: the per-backend view shows that one only, while the
# instance total keeps all four.
foreach my $how ('remote', 'all')
{
	$base = calls();
	my $t = $node->background_psql('postgres');
	$t->query_safe('SET pg_wait_event_tracing.capture = stats');
	my $tpid = $t->query_safe('SELECT pg_backend_pid()');
	$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
	$t->query_until(qr/started/,
		"\\echo started\nSELECT test_wait_totals_emit_hold(4);\n");
	$node->poll_query_until('postgres', 'SELECT test_wait_totals_holding()')
	  or die "$how reset: the emitter never reached its CPU stretch";
	is(calls(), $base + 3,
		"$how reset: 3 completions published while the target holds the 4th");

	if ($how eq 'remote')
	{
		$node->safe_psql('postgres', "SELECT pg_stat_reset_wait_event_timing($tpid)");
	}
	else
	{
		$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_timing_all()');
	}

	$node->safe_psql('postgres', 'SELECT test_wait_totals_release()');
	$t->query_safe('SELECT 1');    # returns once the statement is over
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $base + 4 FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'")
	  or die "$how reset: the pending completion was lost";
	usleep(300_000);
	is(calls(), $base + 4, "$how reset: instance total kept all 4, once each");
	is($legacy->($tpid), '1',
		"$how reset: per-backend view restarted, holding the flushed one only");
	$t->quit;
}

$node->stop;
done_testing();
