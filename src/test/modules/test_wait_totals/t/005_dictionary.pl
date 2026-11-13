# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: the per-ledger LWLock tranche
# dictionary.  Each process admits at most pg_wait_event_tracing.max_tranches
# distinct tranches (the minimum, 16, here); every admitted tranche must keep
# being found however full the table is, and tranches that do not fit go to
# the LWLock overflow pair with both their count and their time.
#
# 9. - Real contention on a named test tranche is found and counted.
#    - A process fills its dictionary to capacity; its admitted keys are
#      still found afterwards; further tranches go to overflow with count
#      and time; no key moves between dictionary and overflow.
#    - A new process reusing the slot does not clear the dictionary.

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
pg_wait_event_tracing.capture = 'stats'
pg_wait_event_tracing.max_tranches = 16
debug_parallel_query = off
max_connections = 12
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

use constant LWLOCK_BASE => 16777216;    # 0x01000000, the LWLock class

sub key_calls
{
	my ($key) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
		 WHERE event_key = $key");
}

sub overflow
{
	# lwlock overflow calls and a flag whether its time is positive
	return $node->safe_psql('postgres',
		"SELECT lwlock_overflow_calls, lwlock_overflow_time_ms > 0
		 FROM pg_stat_wait_event_totals_info");
}

sub wait_key_calls
{
	my ($key, $want, $what) = @_;
	$node->poll_query_until('postgres',
		"SELECT coalesce(sum(calls), 0) = $want FROM pg_stat_wait_event_totals
		 WHERE event_key = $key")
	  or die "$what: key $key never reached $want calls (now "
	  . key_calls($key) . ')';
}

# ---------------------------------------------------------------------
# Real contention on a named test tranche.
# ---------------------------------------------------------------------
my $lock_tranche = $node->safe_psql('postgres', 'SELECT test_wait_totals_lock_tranche(0)');
my $lock_key = LWLOCK_BASE + $lock_tranche;

my $holder = $node->background_psql('postgres');
$holder->query_until(qr/holding/,
	"\\echo holding\nSELECT test_wait_totals_lock_hold(0, 1, 30000);\n");
$node->poll_query_until('postgres', 'SELECT test_wait_totals_lock_holder(0) <> 0')
  or die 'the holder never took the test lock';

# The waiter blocks on the held lock; the holder lets go once it sees it.
$node->safe_psql('postgres', 'SELECT test_wait_totals_lock_wait(0)');
is($holder->query_safe('SELECT 1'), "1\n1",
	'the holder saw one waiter, released, and the session is usable again');
$holder->quit;

$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_wait_event_totals
	 WHERE event_key = $lock_key AND wait_event = 'test_wait_totals_lock_0'
	   AND wait_event_type = 'LWLock' AND calls >= 1 AND total_time_ms > 0")
  or die 'the contended wait on the named test tranche was not counted';
pass('a contended wait on a named test tranche is found and counted');

# ---------------------------------------------------------------------
# Fill one process's dictionary to capacity.  A dictionary is never cleared,
# so any real LWLock wait recorded in the process (or in an earlier owner of
# its slot) would take a cell.  Capture is therefore off on this second
# server, and the helper switches it on only around the synthetic emissions,
# with no SQL and no wait in between: nothing else is ever recorded, and the
# overflow pair changes only by these emissions.
# ---------------------------------------------------------------------
$node->stop;
$node = PostgreSQL::Test::Cluster->new('fill');
TwtUtil::harden($node);
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.max_tranches = 16
debug_parallel_query = off
max_connections = 12
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

my $s = $node->background_psql('postgres');
my $spn = $s->query_safe('SELECT test_wait_totals_procnumber()');
my ($ov_calls0) = split /\|/, overflow();

# 16 admitted tranches (new in this process), then 4 more that must overflow,
# then the first 16 again (still found), then the 4 again (still overflow).
my $fresh = $node->safe_psql('postgres', "SELECT array_to_string(test_wait_totals_new_tranches(16, 'adm'), ' ')");
my $fresh_x = $node->safe_psql('postgres', "SELECT array_to_string(test_wait_totals_new_tranches(4, 'ovf'), ' ')");
my @adm = split / /, $fresh;
my @ovf = split / /, $fresh_x;
my $adm_arr = 'ARRAY[' . join(',', @adm) . ']';
my $ovf_arr = 'ARRAY[' . join(',', @ovf) . ']';

$s->query_safe("SELECT test_wait_totals_emit_with_capture('stats', $adm_arr, 2)");
$s->query_safe("SELECT test_wait_totals_emit_with_capture('stats', $ovf_arr, 3)");
$s->query_safe("SELECT test_wait_totals_emit_with_capture('stats', $adm_arr, 3)");
$s->query_safe("SELECT test_wait_totals_emit_with_capture('stats', $ovf_arr, 1)");

foreach my $id (@adm)
{
	wait_key_calls(LWLOCK_BASE + $id, 5, 'admitted key');
}
pass('keys admitted before the table was full are still found after it');

foreach my $id (@ovf)
{
	is(key_calls(LWLOCK_BASE + $id), '0',
		"tranche $id did not fit: no dictionary row of its own");
}

$node->poll_query_until('postgres',
	"SELECT lwlock_overflow_calls = " . ($ov_calls0 + 16) . " AND lwlock_overflow_time_ms > 0
	 FROM pg_stat_wait_event_totals_info")
  or die 'overflow did not receive the 16 completions of the 4 extra tranches (now '
  . overflow() . ', base ' . $ov_calls0 . ')';
pass('tranches that did not fit are in the overflow pair with count and time');
my ($ov_after) = split /\|/, overflow();
is($ov_after, $ov_calls0 + 16, 'overflow holds count 4*(3+1) exactly');

# No key moved: the overflow row of the view agrees with the info view, and
# the admitted keys' counts did not change by anything that overflowed.
is($node->safe_psql('postgres',
		"SELECT calls FROM pg_stat_wait_event_totals WHERE event_key = -1"),
	$ov_after, 'the -1 row of the totals view is the LWLock overflow pair');
$s->quit;

# ---------------------------------------------------------------------
# Slot reuse does not clear the dictionary: a new process on the same slot
# still has a full table, so a tranche it has not used yet overflows while
# an admitted one is found.  Connect until the slot is handed out again.
# ---------------------------------------------------------------------
my $reuse;
for my $i (1 .. 30)
{
	my $t = $node->background_psql('postgres');
	my $pn = $t->query_safe('SELECT test_wait_totals_procnumber()');
	if ($pn eq $spn)
	{
		$reuse = $t;
		last;
	}
	$t->quit;
}
ok(defined $reuse, "got slot $spn again");

my ($ov_before) = split /\|/, overflow();
$reuse->query_safe("SELECT test_wait_totals_emit_with_capture('stats', $ovf_arr, 1)");
$reuse->query_safe("SELECT test_wait_totals_emit_with_capture('stats', ARRAY[$adm[0]], 1)");
wait_key_calls(LWLOCK_BASE + $adm[0], 6, 'slot reuse: admitted key');
my ($ov_now) = split /\|/, overflow();
is($ov_now, $ov_before + 4,
	'slot reuse: the previous owner\'s full dictionary was not cleared (4 tranches overflowed)');
foreach my $id (@ovf)
{
	is(key_calls(LWLOCK_BASE + $id), '0', "slot reuse: tranche $id still has no row");
}
$reuse->quit;

$node->stop;
done_testing();
