# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: deferred accounting (v11 patch 0004 fixup; see
# DECISION-deferred-accounting.md).  pwet_wait_end_impl() no longer accounts
# a completed wait immediately: it stashes it in a one-slot, backend-local
# pending buffer, applied later by pwet_flush_pending() at the next timed
# wait or one of several other ordering/lifetime points (every SQL reader
# of a backend's own data, a marker write, a capture change, a reset, a
# release/orphan, and process exit).  This file exercises the one
# consequence a single-session regress script cannot: bounded visibility
# latency for a CROSS-BACKEND reader, and the two corners that latency
# touches:
#
#   (a) a wait completed by session A, immediately followed by ~2 seconds
#       of pure CPU work with no further wait of its OWN in it, becomes
#       visible to session B once that one statement finishes (in trace
#       mode, ExecutorEnd's own flush-before-marker; either way, well
#       before A next goes idle).  The decision document promises a
#       BOUNDED delay, not invisibility in the meantime: this file does
#       not assert that B sees nothing before the statement finishes,
#       because A performing some OTHER timed wait during the same
#       statement -- outside this test's control, and observed in
#       practice on at least one platform -- would flush the record
#       earlier, which is equally correct and not a bug;
#   (b) when A then exits with nothing further pending of its own, the
#       ALREADY-flushed wait is not the interesting case -- this instead
#       confirms that ordinary exit cleanup (pwet_before_shmem_exit()'s
#       own flush, then pwet_orphan_trace()'s) does not somehow lose or
#       duplicate a wait that a peer's own read had already flushed
#       earlier, complementing t/005_orphan_reuse.pl's own, differently-
#       timed orphan checks (there, the record is still pending at exit;
#       here, it never is).
#
# A third, independent case (marker ordering in trace mode) closes with a
# statement-boundary check: a completed wait must be flushed into the
# ring before the marker that follows it, even though the two are now
# produced by different code paths (pwet_wait_begin_impl()'s flush for an
# ordinary inter-statement gap vs. post_parse_analyze's flush-before-
# marker for the next statement's own QueryStart -- see
# pwet_flush_pending()'s comment for the full list of call sites).

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
debug_parallel_query = off
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');

# ---------------------------------------------------------------------
# (a)/(b): cross-backend visibility gap while A computes, closing once
# A's long statement finishes; A's exit afterward does not lose or
# duplicate anything.  capture = trace throughout, so pg_stat_wait_event_
# timing (trace "implies stats") covers the visibility-gap check and
# pg_get_wait_event_trace() covers the post-exit orphan check, in the
# same session, matching the scenario as a single continuous story.
# ---------------------------------------------------------------------
my $A = $node->background_psql('postgres');
$A->query_safe('SET pg_wait_event_tracing.capture = trace;');
my $a_pid = $A->query_safe('SELECT pg_backend_pid();');

# A's ProcNumber, needed later to read its ring post-mortem.  Looked up
# via the backend id set, not pg_stat_wait_event_timing (which would
# still be empty here: A has not completed a wait yet, so it has no row
# there until the statement below runs).
my $a_procnumber = $A->query_safe(
	'SELECT id FROM pg_stat_get_backend_idset() AS id '
	  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();');

# One wait, then ~2 seconds of pure CPU work with no wait event in it at
# all, both inside the body of ONE plain SQL statement -- deliberately
# NOT a PL/pgSQL DO block: a plpgsql PERFORM (the only way to call
# pg_sleep() from inside one) always runs through SPI, which means its
# own separate, nested ExecutorStart/ExecutorEnd -- and so, in trace
# mode, this module's own flush-before-marker at THAT ExecutorEnd -- would
# flush the wait immediately after the PERFORM returns, before any
# surrounding loop even started.  A single top-level statement instead
# has exactly one ExecutorStart/ExecutorEnd pair for the whole thing:
# pg_sleep() runs (and completes, becoming pending) while evaluating the
# target list, then the count(*) subquery runs entirely inside the SAME
# executor invocation, and only THAT statement's own ExecutorEnd -- once
# everything is done -- triggers a flush.  AND's left-to-right, short-
# circuiting evaluation (only reached because pg_sleep() IS NULL is true)
# is what guarantees the sleep completes before the counting starts.
# generate_series(1, 30_000_000) is calibrated to run several seconds on
# this module's own debugoptimized+cassert build (the only kind these
# suites run under; see the module's own build instructions), giving
# poll_query_until below something real to poll for rather than finding
# the record already flushed on its very first check.
my $cpu_bound_rows = 30_000_000;
$A->query_until(
	qr/deferred_flush_started/, qq(
\\echo deferred_flush_started
SELECT pg_sleep(0.05) IS NULL AND
       (SELECT count(*) FROM generate_series(1, $cpu_bound_rows)) IS NOT NULL;
));

# B, a completely separate session, does NOT check here that A's PgSleep
# wait is invisible yet: DECISION-deferred-accounting.md promises only a
# bounded visibility delay, never that a cross-backend reader sees
# nothing in the meantime.  A itself may incur some OTHER timed wait
# while evaluating this same statement -- nothing in its text rules that
# out, and it has been observed in practice on at least one platform (a
# wait bound up in generate_series()'s own execution, or in this
# process's ordinary background activity) -- and any such wait's own
# wait_begin would flush the pending PgSleep record right then, well
# before the statement finishes.  That is not a bug: it only makes the
# delay shorter than the worst case this test is about to confirm, so
# asserting non-visibility here would be asserting a guarantee the
# module never made, flaky on any platform where it happens to be false.
#
# Once A's statement finishes, its own ExecutorEnd flushes the pending
# PgSleep record (trace mode's flush-before-marker, ahead of writing that
# statement's own ExecEnd marker).  Poll rather than a fixed sleep: this
# is the actual event under test, not a guessed delay.
$node->poll_query_until(
	'postgres',
	"SELECT count(*) > 0 FROM pg_stat_wait_event_timing "
	  . "WHERE pid = $a_pid AND wait_event = 'PgSleep';"
) or die "A's PgSleep wait never became visible to B after A's statement finished";

pass("A's completed wait becomes visible to B once its long statement finishes");

# A exits with nothing further pending of its own (the visibility check
# above already consumed/flushed the record, well before this point).
# Ordinary exit cleanup must not disturb it: the PgSleep record already
# flushed is still in the now-orphaned ring, neither lost nor duplicated.
# Not an exact count, though: pg_sleep() loops, calling WaitLatch again
# until its own clock says the requested time is up, and on some
# platforms (seen on Windows in CI) the latch timeout and that clock can
# disagree, so a single pg_sleep() call can record more than one PgSleep
# wait (see t/002_ownership.pl's comment on the same behaviour). The
# module is right to count every one of them, so this only checks "at
# least one", never an exact count.
$A->quit;
$node->poll_query_until(
	'postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $a_pid);"
) or die "backend $a_pid did not disappear from pg_stat_activity";

cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_get_wait_event_trace($a_procnumber) "
		  . "WHERE wait_event = 'PgSleep';"
	),
	'>', 0,
	"A's orphaned trace ring still holds at least one PgSleep wait "
	  . "after A's exit");

# ---------------------------------------------------------------------
# Marker ordering: a wait completed just before a statement boundary must
# be flushed into the ring before that statement's own QueryStart marker,
# even though the two are produced by different flush call sites
# (pwet_wait_begin_impl()'s own flush for the ordinary inter-statement
# gap, post_parse_analyze's flush-before-marker for the QueryStart).  A
# real client-side pause (as in t/012_trace_markers.pl) forces a genuine
# ClientRead wait between the two statements, so the ordering is
# exercised by the SAME mechanism a real idle gap would use, not a
# same-message shortcut.
#
# Every statement issued to read the ring is itself self-referential
# (post_parse_analyze/ExecutorStart write its own QueryStart+ExecStart
# before its body runs) -- same caveat sql/pg_wait_event_tracing_trace.sql
# documents at length -- so the mark-fetching statement's own trailing
# ExecEnd/TxnCommit, and the observing statement's own leading
# QueryStart/ExecStart, both land inside the naive "seq > mark" window.
# The standard [3:count(*)-2] trim removes exactly those, in order,
# leaving only what the statements under test actually wrote; Idle is
# filtered out separately since whether it fires is not this test's
# concern.  What is checked afterward, in Perl, is only the ordering
# invariant: a real QueryStart -- there are two in the trimmed sequence:
# the PgSleep statement's own (necessarily before its wait, no news
# there) and the following "SELECT 1"'s (the one actually under test) --
# appears strictly after the single PgSleep wait record.
# ---------------------------------------------------------------------
my $B = $node->background_psql('postgres');
$B->query_safe('SET pg_wait_event_tracing.capture = trace;');
my $mark = $B->query_safe(
	'SELECT coalesce(max(seq), -1) FROM pg_backend_wait_event_trace;');
$B->query_safe('SELECT pg_sleep(0.02);');
sleep(2);
$B->query_safe('SELECT 1;');

my $trimmed = $B->query_safe(
	"SELECT array_to_string("
	  . "(array_agg(wait_event ORDER BY seq))[3:count(*)-2], ',') "
	  . "FROM pg_backend_wait_event_trace "
	  . "WHERE seq > $mark AND wait_event <> 'Idle';");
my @events = split /,/, $trimmed;
my ($pgsleep_idx) = grep { $events[$_] eq 'PgSleep' } 0 .. $#events;
ok(defined $pgsleep_idx, "the PgSleep wait record is present in the ring")
  or diag("trimmed ring contents: $trimmed");
my $querystart_after = defined $pgsleep_idx
  && $pgsleep_idx < $#events
  && grep { $_ eq 'QueryStart' } @events[($pgsleep_idx + 1) .. $#events];
ok($querystart_after,
	"the pending wait is flushed into the ring before the next "
	  . "statement's own QueryStart marker")
  or diag("trimmed ring contents: $trimmed");

$B->quit;
$node->stop;

done_testing();
