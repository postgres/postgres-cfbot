# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: the lifetime ledger and its seqlock reader, checked
# deterministically through assert-build-only entry points (declared here,
# never in the installed script; skipped on builds without assertions).  No
# production hook is blocked or instrumented; timing is never asserted.
#
#  - dictionary: 16 colliding keys admitted, all still found when full, one
#    more goes to overflow with count and time, nothing moves;
#  - a complete writer bracket landing inside the reader's copy makes the
#    reader reject and retry the copy exactly once;
#  - an odd changecount makes the reader retry until the writer finishes;
#  - a writer stalled for good (odd count) cannot make a reader uncancellable;
#  - counters crossing 2^32, changecount rollover;
#  - a counter that would exceed bigint makes the ledger invalid: totals
#    ERROR, the info view still shows it, a per-backend reset-all does not
#    repair it;
#  - sums over ledgers that exceed bigint ERROR the totals.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'test entry points exist only in assert-enabled builds'
  unless check_pg_config('#define USE_ASSERT_CHECKING 1');

sub new_node
{
	my ($name) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf(
		'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
pg_wait_event_tracing.capture = 'stats'
pg_wait_event_tracing.max_tranches = 16
debug_parallel_query = off
));
	$node->start;
	$node->safe_psql(
		'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE EXTENSION test_wait_totals;
CREATE FUNCTION pwet_test_ledger_dictionary() RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_ledger_dictionary' LANGUAGE C;
CREATE FUNCTION pwet_test_ledger_set(bigint, bigint, bigint) RETURNS void
  AS 'pg_wait_event_tracing', 'pwet_test_ledger_set' LANGUAGE C;
CREATE FUNCTION pwet_test_ledger_changecount(int, bigint) RETURNS bigint
  AS 'pg_wait_event_tracing', 'pwet_test_ledger_changecount' LANGUAGE C;
CREATE FUNCTION pwet_test_ledger_torn_read(bigint, OUT retries int, OUT calls bigint)
  AS 'pg_wait_event_tracing', 'pwet_test_ledger_torn_read' LANGUAGE C;
CREATE FUNCTION pwet_test_ledger_odd_read(bigint, int, OUT retries int, OUT calls bigint)
  AS 'pg_wait_event_tracing', 'pwet_test_ledger_odd_read' LANGUAGE C;
));
	return $node;
}

my $node = new_node('main');
my $TOTAL = "SELECT coalesce(sum(calls), 0) FROM pg_stat_wait_event_totals
			 WHERE wait_event = 'TestWaitTotals'";

my ($ret, $out, $err) = $node->psql('postgres', 'SELECT pwet_test_ledger_dictionary()');
is($ret, 0, 'ledger dictionary: collisions, full table, overflow') or diag $err;

my $s = $node->background_psql('postgres');
my $key = $s->query_safe('SELECT test_wait_totals_event_key()');

# Torn copy: one complete writer bracket lands inside the copy.
$s->query_safe('SELECT test_wait_totals_emit(4)');
is($s->query_safe($TOTAL), '4', 'four calls before the torn read');
is($s->query_safe("SELECT retries || ',' || calls FROM pwet_test_ledger_torn_read($key)"),
	'1,5', 'torn copy rejected once; the accepted copy has the extra call');

# Odd count: the reader retries until the stalled writer finishes.
is($s->query_safe("SELECT retries || ',' || calls FROM pwet_test_ledger_odd_read($key, 7)"),
	'7,5', 'odd changecount: reader retried until the writer finished');

# A writer stalled for good: a spare slot's count stays odd.  The reader
# retries but stays cancellable.
my $spare = $node->safe_psql('postgres',
	'SELECT ledger_slots - 1 FROM pg_stat_wait_event_totals_info');
my $cc = $node->safe_psql('postgres',
	"SELECT pwet_test_ledger_changecount($spare, -1)");
is($cc % 2, 0, 'spare slot count is even');
$node->safe_psql('postgres',
	"SELECT pwet_test_ledger_changecount($spare, " . ($cc + 1) . ")");
# The external timeout makes a reader that lost its interrupt check fail
# fast instead of hanging; the ledger is restored before anything else.
my $timed_out = 0;
my ($r2, $o2, $e2) = $node->psql('postgres',
	"SET statement_timeout = 500; SELECT count(*) FROM pg_stat_wait_event_totals",
	timeout => 30, timed_out => \$timed_out);
$node->safe_psql('postgres',
	"SELECT pwet_test_ledger_changecount($spare, " . ($cc + 2) . ")");
ok(!$timed_out, 'the reader of a stalled ledger returns instead of hanging');
isnt($r2, 0, 'reader of a stalled ledger fails');
like($e2, qr/statement timeout/, 'it was cancelled by statement_timeout');
is($node->safe_psql('postgres', 'SELECT count(*) > 0 FROM pg_stat_wait_event_totals'),
	't', 'reads work again once the writer finished');

# Counters crossing 2^32.
$s->query_safe("SELECT pwet_test_ledger_set($key, 4294967290, 4294967290000)");
$s->query_safe('SELECT test_wait_totals_emit(10)');
is($s->query_safe($TOTAL), '4294967300', 'calls cross 2^32 exactly');
cmp_ok($s->query_safe(
		"SELECT total_time_ms FROM pg_stat_wait_event_totals WHERE wait_event = 'TestWaitTotals'"),
	'>', 4294967.29, 'total time crosses 2^32 ns-units too');

# changecount rollover.
$s->query_safe('SELECT pwet_test_ledger_set(' . $key . ', 0, 0)');
my $mypn = $s->query_safe('SELECT test_wait_totals_procnumber()');
$s->query_safe("SELECT pwet_test_ledger_changecount($mypn, 4294967292)");
$s->query_safe('SELECT test_wait_totals_emit(6)');
is($s->query_safe($TOTAL), '6', 'totals right across the rollover');
my $after = $s->query_safe("SELECT pwet_test_ledger_changecount($mypn, -1)");
cmp_ok($after, '<', 100, "changecount wrapped around (now $after)");
is($after % 2, 0, 'changecount even after rollover');

# Invalid ledger: a counter would exceed bigint.
$s->query_safe("SELECT pwet_test_ledger_set($key, 9223372036854775807, 0)");
$s->query_safe('SELECT test_wait_totals_emit(1)');
($ret, $out, $err) = $node->psql('postgres', 'SELECT * FROM pg_stat_wait_event_totals');
isnt($ret, 0, 'totals ERROR with an invalid ledger');
like($err, qr/lifetime ledger.*exceeded the bigint range/, 'clear message');
is($node->safe_psql('postgres', 'SELECT invalid_ledgers FROM pg_stat_wait_event_totals_info'),
	'1', 'the info view still shows the invalid ledger');
$node->safe_psql('postgres', 'SELECT pg_stat_reset_wait_event_timing_all()');
$s->query_safe('SELECT pg_stat_reset_wait_event_timing()');
is($node->safe_psql('postgres', 'SELECT invalid_ledgers FROM pg_stat_wait_event_totals_info'),
	'1', 'reset cannot repair an invalid ledger');
($ret) = $node->psql('postgres', 'SELECT * FROM pg_stat_wait_event_totals');
isnt($ret, 0, 'totals still ERROR after the resets');
$s->quit;
$node->stop;

# Sums over ledgers exceeding bigint.
$node = new_node('sum');
foreach my $i (1, 2)
{
	$node->safe_psql('postgres',
		'SELECT pwet_test_ledger_set(-1, 4611686018427387904, 5)');
}
($ret, $out, $err) = $node->psql('postgres', 'SELECT * FROM pg_stat_wait_event_totals');
isnt($ret, 0, 'sum over ledgers beyond bigint: totals ERROR');
like($err, qr/exceed the bigint range/, 'sum overflow message');
is($node->safe_psql('postgres',
		'SELECT lwlock_overflow_calls IS NULL AND invalid_ledgers = 0
		 FROM pg_stat_wait_event_totals_info'),
	't', 'info shows NULL for the overflowing sum and no invalid ledger');
$node->stop;

done_testing();
