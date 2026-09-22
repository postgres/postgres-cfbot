# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: cross-backend reads against structural changes.
#
# Producers record synthetic LWLock waits on tranches of their own (named
# after their pid) through the real timed wait-event hooks, while resetting
# themselves; other sessions reset the producers (single and all) and still
# others read pg_stat_wait_event_timing and the overflow view in a loop.
# Needs the assert-build-only test entry points (skipped otherwise).
#
# max_tranches is the minimum (16) and each producer uses 20 tranches, so
# every producer's table is full after a reset and the fix 1 path (lookup of
# an admitted tranche in a full table) is exercised constantly.
#
# What is asserted:
#  - nothing errors, every session finishes (no reader spins forever);
#  - no row is attributed to a backend other than the one that produced it
#    (the tranche name carries the producer's pid);
#  - one producer (never reset by anyone else) checks its OWN rows exactly:
#    with a full table, after each of its own resets, exactly 16 of its
#    tranches are admitted, each with the exact count, and the 4 refused
#    ones appear in lwlock_overflow_count -- admitted tranches keep being
#    counted.
#
# What is deliberately not asserted: relations among the counter fields of
# a row (count, total, max, histogram) -- they are approximate by design
# and a cross-backend reader can see them at slightly different moments --
# and which of two tranches of ONE producer a row's counters belong to; the
# deterministic checks for the latter are in t/008_lwlock_table.pl.  This
# test is a liveness, no-crash and no-cross-attribution test.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'test entry points exist only in assert-enabled builds'
  unless check_pg_config('#define USE_ASSERT_CHECKING 1');

my $duration = 12;    # seconds
my $ntranches = 20;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.capture = stats
pg_wait_event_tracing.max_tranches = 16
debug_parallel_query = off
max_connections = 40
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');
$node->safe_psql(
	'postgres', q(
CREATE FUNCTION pwet_test_new_tranches(text, int) RETURNS int4[]
  AS 'pg_wait_event_tracing', 'pwet_test_new_tranches' LANGUAGE C;
CREATE FUNCTION pwet_test_waits(int4[], int, int) RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_waits' LANGUAGE C;
));

# Producers: waits on their own tranches, resetting themselves now and
# then.  The pid is known only inside the session, so tranches are named
# there; the tag is stress_<pid>_<k>.
my $producer_sql = qq!
DO \$\$
DECLARE
  ids int4[];
  t0 timestamptz := clock_timestamp();
  n int := 0;
BEGIN
  ids := pwet_test_new_tranches('stress_' || pg_backend_pid() || '_',
                                $ntranches);
  WHILE clock_timestamp() < t0 + interval '$duration seconds' LOOP
    PERFORM pwet_test_waits(ids, 3, n % $ntranches);
    n := n + 1;
    IF n % 5 = 0 THEN
      PERFORM pg_stat_reset_wait_event_timing(pg_backend_pid());
    END IF;
  END LOOP;
END \$\$;
!;

# The exact-checking producer: nobody else resets it.  After each of its
# own resets it waits 3 rounds over all tranches (the table has room for 16
# of the 20) and then reads its own rows, which are exact for the owner.
my $checker_sql = qq!
DO \$\$
DECLARE
  ids int4[];
  t0 timestamptz := clock_timestamp();
  nrows int;
  nbad int;
  ovf bigint;
  iter int := 0;
  fails int := 0;
BEGIN
  ids := pwet_test_new_tranches('exact_' || pg_backend_pid() || '_',
                                $ntranches);
  WHILE clock_timestamp() < t0 + interval '$duration seconds' LOOP
    PERFORM pg_stat_reset_wait_event_timing(pg_backend_pid());
    PERFORM pwet_test_waits(ids, 3, iter % $ntranches);
    SELECT count(*), count(*) FILTER (WHERE calls <> 3)
      INTO nrows, nbad
      FROM pg_stat_wait_event_timing
      WHERE pid = pg_backend_pid() AND wait_event LIKE 'exact_%';
    SELECT lwlock_overflow_count INTO ovf
      FROM pg_stat_wait_event_timing_overflow
      WHERE pid = pg_backend_pid();
    -- A real LWLock wait of this backend between the reset and the read
    -- would legitimately take a slot; a genuine bug repeats every time, so
    -- only consecutive failures count.
    IF nrows <> 16 OR nbad <> 0 OR ovf <> 12 THEN
      fails := fails + 1;
      IF fails >= 3 THEN
        RAISE EXCEPTION 'exact check failed: rows=% badcount=% overflow=%',
          nrows, nbad, ovf;
      END IF;
    ELSE
      fails := 0;
    END IF;
    iter := iter + 1;
  END LOOP;
  RAISE NOTICE 'exact iterations: %', iter;
END \$\$;
!;

# Remote resetters: single-target and reset-all.
my $resetter_sql = qq!
DO \$\$
DECLARE
  t0 timestamptz := clock_timestamp();
  p int4;
BEGIN
  WHILE clock_timestamp() < t0 + interval '$duration seconds' LOOP
    -- only the plain producers: the exact-checking one is never reset
    -- by anybody else
    FOR p IN SELECT pid FROM pg_stat_activity
             WHERE pid <> pg_backend_pid()
               AND query LIKE '%pwet_test_new_tranches(''stress_''%' LOOP
      BEGIN
        PERFORM pg_stat_reset_wait_event_timing(p);
      EXCEPTION WHEN OTHERS THEN NULL;
      END;
    END LOOP;
    PERFORM pg_sleep(0.005);
  END LOOP;
END \$\$;
!;

# Reset-all would also reset the exact-checking producer, so it gets its own
# phase after the checker has finished (below).

# Readers: cross-backend sweeps; any row for a stress_/exact_ tranche must
# belong to the pid embedded in the tranche name.
my $reader_sql = qq!
DO \$\$
DECLARE
  t0 timestamptz := clock_timestamp();
  nbad int;
  loops int := 0;
BEGIN
  WHILE clock_timestamp() < t0 + interval '$duration seconds' LOOP
    SELECT count(*) INTO nbad FROM pg_stat_wait_event_timing
      WHERE wait_event ~ '^(stress|exact)_[0-9]+_[0-9]+\$'
        AND CASE WHEN wait_event ~ '^(stress|exact)_[0-9]+_[0-9]+\$'
                 THEN split_part(wait_event, '_', 2)::int END <> pid;
    IF nbad <> 0 THEN
      RAISE EXCEPTION 'cross-attributed rows: %', nbad;
    END IF;
    PERFORM count(*) FROM pg_stat_wait_event_timing_overflow;
    loops := loops + 1;
  END LOOP;
  RAISE NOTICE 'reader loops: %', loops;
END \$\$;
!;

my @sessions;
my %kind;

foreach my $spec (
	[ producer => $producer_sql ],
	[ producer => $producer_sql ],
	[ checker  => $checker_sql ],
	[ resetter => $resetter_sql ],
	[ reader   => $reader_sql ],
	[ reader   => $reader_sql ],)
{
	my ($kind, $sql) = @$spec;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe("SET pg_wait_event_tracing.capture = stats;")
	  if $kind ne 'reader';
	push @sessions, [ $kind, $s, $sql ];
}

# Start them all, then collect.
foreach my $e (@sessions)
{
	my ($kind, $s, $sql) = @$e;
	$s->query_until(qr/started/, "\\echo started\n$sql\n");
}

foreach my $e (@sessions)
{
	my ($kind, $s, $sql) = @$e;
	my $out = $s->query("SELECT 'finished';");
	like($out, qr/finished/, "$kind session finished");
	unlike($s->{stderr} // '', qr/ERROR/, "$kind session raised no error");
	$s->quit;
}

# Reset-all phase: producers running while the superuser resets everyone.
my @phase;
foreach my $i (1 .. 3)
{
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe("SET pg_wait_event_tracing.capture = stats;");
	(my $sql = $producer_sql) =~ s/$duration seconds/4 seconds/;
	$s->query_until(qr/started/, "\\echo started\n$sql\n");
	push @phase, $s;
}
my $all = $node->background_psql('postgres', on_error_stop => 0);
$all->query_until(
	qr/started/, qq!\\echo started
DO \$\$
DECLARE t0 timestamptz := clock_timestamp();
BEGIN
  WHILE clock_timestamp() < t0 + interval '4 seconds' LOOP
    PERFORM pg_stat_reset_wait_event_timing_all();
    PERFORM count(*) FROM pg_stat_wait_event_timing;
    PERFORM pg_sleep(0.005);
  END LOOP;
END \$\$;
!);
foreach my $s (@phase, $all)
{
	like($s->query("SELECT 'finished';"), qr/finished/,
		"reset-all phase session finished");
	unlike($s->{stderr} // '', qr/ERROR/, "reset-all phase raised no error");
	$s->quit;
}

ok($node->safe_psql('postgres', 'SELECT 1') eq '1', 'server still alive');
$node->stop;
done_testing();
