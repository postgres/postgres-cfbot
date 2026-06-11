# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the position-encoded identity seqlock that protects cross-backend
# reads of the wait-event trace ring (wait_event_capture = trace).
#
# The hazard: the trace writer advances write_pos and only then stamps the
# record's seq.  A cross-backend reader that observes the new write_pos
# before the seq store has propagated sees, at the in-flight ring slot, the
# PREVIOUS cycle's record -- complete, with an even seq.  A parity-only
# seqlock would accept it and emit a stale record attributed to the wrong
# ring index; the identity check (seq must equal the writer's completion
# value for that exact position) must reject it.
#
# That window is unobservable on TSO hardware without instrumentation, so
# the writer carries INJECTION_POINT("wait-event-trace-after-write-pos")
# between the write_pos advance and the seq stamp.  This test:
#
#   1. fills and wraps a minimum-size ring (8kB = 256 records), so every
#      slot holds a complete record from the previous cycle;
#   2. wedges the writer at the injection point, mid-record;
#   3. reads the ring cross-backend: the reader must return exactly
#      ring_size - 1 records, skipping the in-flight slot whose stale
#      prior-cycle record a parity-only check would have emitted;
#   4. releases the writer and verifies the ring reads full again.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $ring_records = 256;			# 8kB ring / 32-byte records

my $node = PostgreSQL::Test::Cluster->new('seqlock');
$node->init;
$node->append_conf(
	'postgresql.conf', q[
wait_event_trace_ring_size = '8kB'
]);
$node->start;

# Skip if the server was not built with --enable-wait-event-timing.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'SET wait_event_capture = trace;');
if ($ret != 0)
{
	$node->stop;
	plan skip_all => 'server not built with --enable-wait-event-timing';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Writer session: enable trace and wrap the ring.  400 pg_sleep calls emit
# at least 400 wait events into a 256-record ring, so every slot holds a
# complete record from the current window.
my $writer = $node->background_psql('postgres');
$writer->query_safe('SET wait_event_capture = trace;');
$writer->query_safe(
	'SELECT count(pg_sleep(0.001)) FROM generate_series(1, 400);');

my $writer_proc = $writer->query_safe(
	'SELECT procnumber FROM pg_stat_get_wait_event_timing(pg_backend_pid())'
	  . ' LIMIT 1;');
chomp $writer_proc;
like($writer_proc, qr/^\d+$/, 'writer reported its procnumber');

# With the ring wrapped and the writer idle, a cross-backend read returns
# exactly ring_size records: every slot is complete and identity-valid.
my $count_full = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_full, $ring_records, 'wrapped ring reads full before the wedge');

# Wedge the writer mid-record: arm the injection point, then send a
# statement.  The arrival of the statement completes the writer's
# ClientRead wait; its trace write advances write_pos and then blocks at
# the injection point, before stamping the record's seq.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('wait-event-trace-after-write-pos', 'wait');"
);
$writer->query_until(
	qr/wedge_sent/, q[
\echo wedge_sent
SELECT 1;
]);
$node->wait_for_event('client backend', 'wait-event-trace-after-write-pos');

# The decisive read: the in-flight slot still holds the previous cycle's
# complete record.  A parity-only seqlock would emit it (ring_size rows,
# one misattributed); the identity check must skip exactly that slot.
my $count_wedged = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_wedged, $ring_records - 1,
	'reader skips the in-flight slot instead of emitting the stale prior-cycle record'
);

# The read is stable and repeatable while the writer is wedged.
my $count_wedged2 = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_wedged2, $count_wedged, 'wedged-ring read is stable');

# Release the writer: detach first so the nested wakeup wait does not
# re-arm, then wake it.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('wait-event-trace-after-write-pos');");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('wait-event-trace-after-write-pos');");

# The writer completes the wedged record (and its pending statement); the
# ring must read full again.
$writer->query_safe("SELECT 'resync';");
my $count_after = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_after, $ring_records, 'ring reads full again after release');

$writer->quit;
$node->stop;

done_testing();
