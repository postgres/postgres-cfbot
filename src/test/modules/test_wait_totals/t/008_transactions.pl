# Copyright (c) 2026, PostgreSQL Global Development Group

# Instance totals of pg_wait_event_tracing: captured transaction completions
# (xact_commit, xact_abort, xact_prepare).  Capture is off cluster-wide, so
# the only transactions counted are those of the one session each case runs
# (it turns capture on itself), and every delta is exact.  A case is one
# fresh connection running one script; the counters are read by this
# observer, whose capture is off, before and after.
#
# Counting follows the callback: COMMIT, ABORT and PREPARE events of the
# top-level transaction, when capture is on at that moment (a SET LOCAL
# value is still in effect then); subtransactions are not counted; COMMIT
# PREPARED and ROLLBACK PREPARED run their own transaction in the resolving
# session, which commits; parallel workers are never counted.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use PostgreSQL::Test::Cluster;
use TwtUtil;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
TwtUtil::harden($node);
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
autovacuum = off
max_prepared_transactions = 5
max_worker_processes = 16
max_parallel_workers = 8
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing');
$node->safe_psql('postgres', 'CREATE EXTENSION test_wait_totals');

sub counters
{
	return $node->safe_psql('postgres',
		'SELECT xact_commit || \',\' || xact_abort || \',\' || xact_prepare
		 FROM pg_stat_wait_event_totals_info');
}

my $ON = 'SET pg_wait_event_tracing.capture = stats;';

# [label, script, expected "commit,abort,prepare" delta]
my @cases = (
	[ 'autocommit statement', "$ON SELECT 1;", '2,0,0' ],
	[ 'explicit commit', "$ON BEGIN; SELECT 1; COMMIT;", '2,0,0' ],
	[ 'read-only transaction', "$ON BEGIN READ ONLY; SELECT 1; COMMIT;", '2,0,0' ],
	[ 'explicit rollback', "$ON BEGIN; SELECT 1; ROLLBACK;", '1,1,0' ],
	[ 'error in autocommit statement', "$ON SELECT 1/0;", '1,1,0' ],
	[ 'error in a transaction block, then ROLLBACK',
		"$ON BEGIN; SELECT 1/0; ROLLBACK;", '1,1,0' ],
	[ 'savepoint rollback is not counted, the commit is',
		"$ON BEGIN; SAVEPOINT s; SELECT 1/0; ROLLBACK TO s; COMMIT;", '2,0,0' ],
	[ 'PREPARE counts as a prepare; COMMIT PREPARED as the resolving commit',
		"$ON BEGIN; SELECT 1; PREPARE TRANSACTION 'pwet_t1'; COMMIT PREPARED 'pwet_t1';",
		'2,0,1' ],
	[ 'ROLLBACK PREPARED is the resolving session\'s own committed transaction',
		"$ON BEGIN; PREPARE TRANSACTION 'pwet_t2'; ROLLBACK PREPARED 'pwet_t2';",
		'2,0,1' ],
	[ 'SET LOCAL capture on: commit counted',
		'BEGIN; SET LOCAL pg_wait_event_tracing.capture = stats; SELECT 1; COMMIT;',
		'1,0,0' ],
	[ 'SET LOCAL capture on: abort counted',
		'BEGIN; SET LOCAL pg_wait_event_tracing.capture = stats; SELECT 1/0; ROLLBACK;',
		'0,1,0' ],
	[ 'SET LOCAL capture off: commit not counted',
		"$ON BEGIN; SET LOCAL pg_wait_event_tracing.capture = off; SELECT 1; COMMIT;",
		'1,0,0' ],
	[ 'SET LOCAL capture off: abort not counted',
		"$ON BEGIN; SET LOCAL pg_wait_event_tracing.capture = off; SELECT 1/0; ROLLBACK;",
		'1,0,0' ],
	[ 'capture turned on inside the transaction: its commit counts',
		'BEGIN; SET pg_wait_event_tracing.capture = stats; SELECT 1; COMMIT;',
		'1,0,0' ],
	[ 'capture turned off inside the transaction: its commit does not count',
		"$ON BEGIN; SET pg_wait_event_tracing.capture = off; COMMIT;",
		'1,0,0' ],
	[ 'trace level counts as well',
		'SET pg_wait_event_tracing.capture = trace; SELECT 1;',
		'2,0,0' ],
	[ 'capture off counts nothing', 'SELECT 1; BEGIN; SELECT 1; ROLLBACK;', '0,0,0' ],
);

foreach my $c (@cases)
{
	my ($label, $script, $want) = @$c;
	my $before = counters();
	$node->psql('postgres', $script, on_error_stop => 0);
	my @b = split /,/, $before;
	my @a = split /,/, counters();
	my $got = join(',', map { $a[$_] - $b[$_] } 0 .. 2);
	is($got, $want, $label);
}

# Parallel query.  A role setting turns capture on for the connection from
# its very start, and parallel workers connect as the same role, so their
# own initialization transactions would be counted too if they were not
# skipped.  The same script runs once with parallel workers and once without
# (max_parallel_workers_per_gather = 0): the deltas must be equal, i.e.
# workers add nothing, and the test shows that the workers really ran the
# emitter (the leader does not participate, so every row ran in a worker).
$node->safe_psql('postgres', 'CREATE TABLE ptab AS SELECT g FROM generate_series(1, 4000) g');
$node->safe_psql('postgres',
	q(CREATE ROLE pwet_cap SUPERUSER LOGIN;
	  ALTER ROLE pwet_cap SET pg_wait_event_tracing.capture = stats;));
sub parallel_run
{
	my ($workers) = @_;
	my $before = counters();
	my $out;
	$node->psql(
		'postgres', qq(
SET debug_parallel_query = off;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = $workers;
SET parallel_leader_participation = off;
SELECT sum(test_wait_totals_emit_row(1)) FROM ptab;
), stdout => \$out, extra_params => [ '-U', 'pwet_cap' ]);
	my @b = split /,/, $before;
	my @a = split /,/, counters();
	return ($out, join(',', map { $a[$_] - $b[$_] } 0 .. 2));
}
my ($out_par, $delta_par) = parallel_run(2);
my ($out_seq, $delta_seq) = parallel_run(0);
like($out_par, qr/\b4000\b/, 'parallel query: every row ran in a worker');
unlike($out_seq, qr/\b4000\b/, 'control without workers: none did');
is($delta_par, $delta_seq,
	'parallel workers add nothing to the counters (delta equals the no-worker run)');
cmp_ok((split /,/, $delta_par)[0], '>=', 7,
	'and the leader\'s init, SET and query transactions were counted');

# Connection-initialization transactions count when capture is enabled for
# the connection from the start, and not otherwise.
{
	my $before = counters();
	$node->psql('postgres', 'SELECT 1');
	is(counters(), $before, 'init transactions of a capture-off connection: none');

	my @b = split /,/, counters();
	{
		local $ENV{PGOPTIONS} = '-c pg_wait_event_tracing.capture=stats';
		$node->psql('postgres', 'SELECT 1');
	}
	my @a = split /,/, counters();
	cmp_ok($a[0] - $b[0], '>=', 2,
		'connection with capture on from the start: init commit(s) plus the statement');
	is($a[1] - $b[1], 0, 'no aborts');
}

$node->stop;
done_testing();
