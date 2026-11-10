# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: coherence of concurrent reads.
# Several sessions emit waits at a high rate while another session reads the
# totals over and over.  The assertions have no timing bounds; they are
# invariants that a torn or stale read would break:
#
#  - successive reads, which do not overlap, are monotonic per field;
#  - within one read, the total time is at least calls times the minimum
#    duration of one wait (every emitted wait spins for at least a
#    microsecond), which a read that paired a newer call count with an older
#    time could not satisfy;
#  - once the writers have exited, the total is exactly what they emitted.
#
# The deterministic seqlock checks (an odd count, a torn copy, a stalled
# writer, counter ranges) are in the module's own t/017_totals_ledger.pl.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.capture = 'stats'
debug_parallel_query = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

my $writers = 4;
my $nsamples = 300;

# Between two samples the reader waits for the writers' progress counter to
# advance, so every pair of samples is separated by known writer progress.
# The writers run until the reader has taken all its samples, and the reader
# starts only once every writer is running, so the reads overlap the writes
# whatever the scheduling.  The watchdog inside the helpers only turns a
# hang into a failure.
$node->safe_psql('postgres', 'SELECT test_wait_totals_gate_reset()');
my @w;
for my $i (1 .. $writers)
{
	my $s = $node->background_psql('postgres');
	$s->query_until(qr/started/,
		"\\echo started\nSELECT test_wait_totals_emit_until_stopped(1);\n");
	push @w, $s;
}

my $reader = $node->background_psql('postgres');
$reader->query_safe("SELECT test_wait_totals_wait_writers($writers)");
$reader->query_safe(q(
CREATE TEMP TABLE snap (i int, calls bigint, ms double precision);
DO $$
DECLARE
	i int := 0;
	c bigint;
	m double precision;
	p bigint := 0;
BEGIN
	LOOP
		-- Writers must have made progress since the previous sample.
		p := test_wait_totals_wait_progress(p);
		SELECT coalesce(sum(calls), 0), coalesce(sum(total_time_ms), 0)
		  INTO c, m
		FROM pg_stat_wait_event_totals WHERE wait_event = 'TestWaitTotals';
		i := i + 1;
		INSERT INTO snap VALUES (i, c, m);
		EXIT WHEN i >= ) . $nsamples . q(;
	END LOOP;
END $$;
));
$reader->query_safe('SELECT test_wait_totals_stop()');

is($reader->query_safe('SELECT count(*) FROM snap'), $nsamples,
	"the reader took $nsamples samples while all writers ran");

is($reader->query_safe(
		'SELECT count(*) FROM (SELECT calls, ms,
		   lag(calls) OVER (ORDER BY i) AS pc, lag(ms) OVER (ORDER BY i) AS pm
		 FROM snap) x WHERE calls < pc OR ms < pm'),
	'0', 'successive reads are monotonic per field');

is($reader->query_safe(
		'SELECT count(*) FROM snap WHERE ms * 1000000 < calls * 1000 * 0.999999'),
	'0', 'every read has total time >= calls * minimum wait duration');

cmp_ok($reader->query_safe('SELECT max(calls) - min(calls) FROM snap'),
	'>', 0, 'the writers made progress while the reader sampled');
$reader->quit;

my $total = 0;
foreach my $s (@w)
{
	my $out = $s->query_safe('SELECT 1');
	my ($n) = split /\n/, $out;
	$total += $n;
	$s->quit;
}

$node->poll_query_until('postgres',
	"SELECT coalesce(sum(calls), 0) = $total FROM pg_stat_wait_event_totals
	 WHERE wait_event = 'TestWaitTotals'")
  or die 'the total never reached what the writers emitted';
is($node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
		 WHERE wait_event = 'TestWaitTotals'"),
	$total, "after the writers exited the total is exactly what they emitted ($total)");

$node->stop;
done_testing();
