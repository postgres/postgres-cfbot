# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the position-encoded identity seqlock that protects cross-backend
# reads of the wait-event trace ring (pg_wait_event_tracing.capture =
# trace).  Ported from v6's
# src/test/modules/test_misc/t/016_wait_event_trace_seqlock.pl onto this
# module's names; same hazard, same assertions.
#
# The hazard: the trace writer advances write_pos and only then stamps
# the record's seq (see the injection point's comment in pwet_wait_end(),
# pg_wait_event_tracing.c).  A cross-backend reader that observes the new
# write_pos before the seq store has propagated sees, at the in-flight
# ring slot, the PREVIOUS cycle's record -- complete, with an even seq.
# A parity-only seqlock would accept it and emit a stale record
# attributed to the wrong ring index; the identity check (seq must equal
# the writer's completion value for that exact position) must reject it
# instead.
#
# That window is unobservable on TSO hardware without instrumentation,
# so the writer carries INJECTION_POINT("pg-wait-event-tracing-trace-
# after-write-pos") between the write_pos advance and the seq stamp --
# compiled in only for an injection-point build, and a no-op even there
# unless a test explicitly attaches an action to it, which is why the
# call is acceptable inside a hook that must not otherwise allocate,
# lock, wait, or ereport (see the comment beside it).  This test:
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
  unless ($ENV{enable_injection_points} // '') eq 'yes';

my $ring_records = 256;			# 8kB ring / 32-byte records

my $node = PostgreSQL::Test::Cluster->new('seqlock');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing, injection_points'
pg_wait_event_tracing.trace_ring_size = '8kB'
debug_parallel_query = off
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

my $point = 'pg-wait-event-tracing-trace-after-write-pos';

# Writer session: enable trace and wrap the ring.  400 pg_sleep calls,
# all inside one statement, emit at least 400 PgSleep wait records into a
# 256-record ring, so every slot holds a complete record from the
# current window by the time the statement finishes.
my $writer = $node->background_psql('postgres');
$writer->query_safe("SET pg_wait_event_tracing.capture = trace;");
$writer->query_safe(
	'SELECT count(pg_sleep(0.001)) FROM generate_series(1, 400);');

my $writer_proc = $writer->query_safe(
	'SELECT procnumber FROM pg_stat_get_wait_event_timing(pg_backend_pid())'
	  . ' LIMIT 1;');
like($writer_proc, qr/^\d+$/, 'writer reported its procnumber');

# With the ring wrapped and the writer idle, a cross-backend read returns
# exactly ring_size records: every slot is complete and identity-valid
# (a mix of wait and query-marker records -- fix 6 added markers on top
# of v6's design -- but the seqlock protocol treats them identically).
my $count_full = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_full, $ring_records, 'wrapped ring reads full before the wedge');

# Wedge the writer mid-record: arm the injection point, then send a
# statement.  The arrival of the statement's bytes completes the
# writer's blocked ClientRead wait; recording that wait's completion
# advances write_pos and then blocks at the injection point, before
# stamping the record's seq.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('$point', 'wait');");
$writer->query_until(
	qr/wedge_sent/, q(
\echo wedge_sent
SELECT 1;
));
$node->wait_for_event('client backend', $point);

# The decisive read: the in-flight slot still holds the previous cycle's
# complete record.  A parity-only seqlock would emit it (ring_size rows,
# one misattributed); the identity check must skip exactly that slot.
my $count_wedged = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_wedged, $ring_records - 1,
	'reader skips the in-flight slot instead of emitting the stale '
	  . 'prior-cycle record');

# The read is stable and repeatable while the writer is wedged.
my $count_wedged2 = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_wedged2, $count_wedged, 'wedged-ring read is stable');

# Release the writer: detach first so the nested wakeup wait does not
# re-arm, then wake it.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('$point');");
$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point');");

# The writer completes the wedged record (and its pending "SELECT 1"
# statement); the ring must read full again.  This query_safe's own
# return value is not meaningful (query_until above left "SELECT 1"'s
# own result unconsumed, ahead of this one in the pipe), only that it
# completes, proving the writer is no longer wedged.
$writer->query_safe("SELECT 'resync';");
my $count_after = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_get_wait_event_trace($writer_proc);");
is($count_after, $ring_records, 'ring reads full again after release');

$writer->quit;
$node->stop;

done_testing();
