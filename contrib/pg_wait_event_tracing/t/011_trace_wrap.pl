# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: trace ring wrap, and reading it while it wraps,
# while it is disabled, and after its owner has exited.
#
# The ring's "seq" column (see pg_get_wait_event_trace() in
# pg_wait_event_tracing.c) is the ABSOLUTE, monotonically increasing
# write position, not a ring-wrapped index, so once a ring has wrapped,
# the surviving records' seq values are exactly the top ring_size
# integers the writer has produced so far: contiguous, with the oldest
# ones (the low seq values) evicted.  That is what "wrapped and
# contiguous" is checked against below, with no need to reproduce the
# writer's own modular indexing in Perl.
#
# pg_sleep(0) is not a real wait on Linux (it returns without ever
# calling WaitLatch), so it would not reliably produce one PgSleep
# record per iteration; pg_sleep(0.001) always does.

use strict;
use warnings FATAL => 'all';

use Time::HiRes qw(usleep);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $ring_records = 256;			# 8kB ring / 32-byte records: the GUC minimum

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.trace_ring_size = '8kB'
debug_parallel_query = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');

my $writer = $node->background_psql('postgres');
$writer->query_safe("SET pg_wait_event_tracing.capture = trace;");
my $writer_pid = $writer->query_safe("SELECT pg_backend_pid();");
my $writer_proc = $writer->query_safe(
	'SELECT procnumber FROM pg_stat_get_wait_event_timing(pg_backend_pid())'
	  . ' LIMIT 1;');
like($writer_proc, qr/^\d+$/, 'writer reported its procnumber');

# Launch, without waiting for it to finish, one statement that produces
# far more wait records than the ring holds: 800 iterations of
# pg_sleep(0.003) is ~2.4s of real waits, comfortably more than the
# handful of concurrent reads below need, and comfortably more than
# ring_records (256) waits to guarantee at least one full wrap.  The
# \echo fires (and is seen by query_until) before the SELECT completes,
# handing control back to this script while the writer is still busy.
$writer->query_until(
	qr/loop_started/, q(
\echo loop_started
SELECT count(pg_sleep(0.003)) FROM generate_series(1, 800);
));

# Concurrent reads while the writer is still (probably) running: on
# every read, whatever the reader sees must have no duplicate seq and no
# gap other than possibly missing the single newest, still-in-flight
# record (never a gap in the middle -- the writer is single-threaded and
# strictly sequential, so only the very last position it is currently
# writing can ever be caught incomplete).  A read is capped at
# ring_records rows by construction (the reader never looks back further
# than one ring's worth of positions).
for my $i (1 .. 5)
{
	my $row = $node->safe_psql(
		'postgres', qq(
		SELECT count(*), count(DISTINCT seq), min(seq), max(seq)
		FROM pg_get_wait_event_trace($writer_proc);
	));
	my ($count, $distinct_count, $min_seq, $max_seq) = split /\|/, $row;

	next unless length($count) && $count > 0;

	is($distinct_count, $count,
		"concurrent read $i: no duplicate seq values");
	is($max_seq - $min_seq + 1, $count,
		"concurrent read $i: no gap other than possibly the newest record");
	cmp_ok($count, '<=', $ring_records,
		"concurrent read $i: never more than the ring's capacity");

	usleep(300_000);
}

# Let the writer's statement actually finish before checking the final,
# settled ring state.
$node->poll_query_until('postgres',
	"SELECT state = 'idle' FROM pg_stat_activity WHERE pid = $writer_pid;"
) or die "writer backend $writer_pid did not go idle";

my $final_row = $node->safe_psql(
	'postgres', qq(
	SELECT count(*), min(seq), max(seq)
	FROM pg_get_wait_event_trace($writer_proc);
));
my ($final_count, $final_min, $final_max) = split /\|/, $final_row;

is($final_count, $ring_records,
	'record count equals the ring capacity once the writer is idle');
is($final_max - $final_min + 1, $final_count,
	'surviving sequence numbers are contiguous');
cmp_ok($final_min, '>', 0,
	'the oldest records (starting at seq 0) were overwritten');

# Reading after the writer disables trace: the ring is freed outright
# (a live step-down, not an exit -- see pwet_release_trace()), so the
# read must succeed and simply come back empty, not error.
$writer->query_safe('SET pg_wait_event_tracing.capture = stats;');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);"),
	'0',
	'reading after the writer disables trace succeeds, and finds nothing'
);

# Reading after the writer exits: re-enable trace, record one more wait
# so the ring is non-empty again, then quit the session outright.  Exit
# orphans the ring instead of freeing it (fix 3; the full reclaim/sweep
# lifecycle has its own dedicated coverage in t/005_orphan_reuse.pl) --
# here only "the read still succeeds" is being checked.
$writer->query_safe('SET pg_wait_event_tracing.capture = trace;');
$writer->query_safe('SELECT pg_sleep(0.01);');
$writer->quit;
$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $writer_pid);"
) or die "writer backend $writer_pid did not disappear from pg_stat_activity";

cmp_ok(
	$node->safe_psql('postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);"),
	'>',
	0,
	'reading after the writer exits succeeds, and finds its last records');

$node->stop;

done_testing();
