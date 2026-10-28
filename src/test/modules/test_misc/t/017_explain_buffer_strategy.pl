# Copyright (c) 2026, PostgreSQL Global Development Group

# Test EXPLAIN reporting of buffer access strategies for table scans,
# including rescans, parallel execution, and cases where no strategy is shown.

use strict;
use warnings FATAL => 'all';

use JSON::PP qw(decode_json);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_buffers = '1MB'
max_parallel_workers_per_gather = 0
});
$node->start;

$node->safe_psql(
	'postgres', qq{
CREATE TABLE test_strategy AS
  SELECT g, repeat('x', 1000) AS payload
  FROM generate_series(1, 4000) AS g;
ALTER TABLE test_strategy SET (parallel_workers = 2);
CREATE INDEX test_strategy_g_idx ON test_strategy (g);
CREATE TABLE test_normal AS SELECT 1 AS g;
});

my $seqscan = explain_json('SELECT * FROM test_strategy');
is($seqscan->{'Node Type'}, 'Seq Scan', 'sequential scan plan');
check_strategy($seqscan, 'bulkread', 'sequential scan');

my $normal_scan = explain_json('SELECT * FROM test_normal');
is($normal_scan->{'Node Type'}, 'Seq Scan', 'small sequential scan plan');
check_strategy($normal_scan, 'normal', 'small sequential scan');

my $never_executed_plan = decode_json(
	$node->safe_psql(
		'postgres', qq{
SET plan_cache_mode = force_generic_plan;
PREPARE never_execute(bool) AS SELECT * FROM test_normal WHERE \$1;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  EXECUTE never_execute(false);
}))->[0]->{'Plan'};
my $never_executed_scan = find_plan_node($never_executed_plan, 'Seq Scan');
ok(defined $never_executed_scan,
	'never-executed plan contains a sequential scan');
is($never_executed_scan->{'Actual Loops'},
	0, 'sequential scan was not executed');
ok( !exists $never_executed_scan->{'Buffer Access Strategies'},
	'strategy is not shown for a never-executed scan');

# Sample scans use the buffer access strategy without a read stream.
my $samplescan =
  explain_json('SELECT * FROM test_strategy TABLESAMPLE SYSTEM (100)');
is($samplescan->{'Node Type'}, 'Sample Scan', 'sample scan plan');
check_strategy($samplescan, 'bulkread', 'sample scan');

my $rescan_plan = explain_json(
	q{
SELECT count(*)
FROM (
  SELECT p
  FROM (VALUES (1, 100::real), (2, 0.5::real)) v(ord, p)
  ORDER BY ord
) o
JOIN LATERAL (
  SELECT *
  FROM test_strategy TABLESAMPLE SYSTEM (o.p) REPEATABLE (0)
) s ON true
});
my $rescan = find_plan_node($rescan_plan, 'Sample Scan');
ok(defined $rescan, 'parameterized sample scan plan');
is($rescan->{'Actual Loops'}, 2, 'parameterized sample scan was rescanned');
is_deeply(
	$rescan->{'Buffer Access Strategies'},
	[ 'normal', 'bulkread' ],
	'parameterized sample scan reports every strategy used');

my $bitmap_plan = decode_json(
	$node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
SET enable_indexscan = off;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  SELECT * FROM test_strategy WHERE g BETWEEN 1 AND 1000;
}))->[0]->{'Plan'};
my $bitmap_scan = find_plan_node($bitmap_plan, 'Bitmap Heap Scan');
ok(defined $bitmap_scan, 'bitmap heap scan plan');
check_strategy($bitmap_scan, 'normal', 'bitmap heap scan');

my $tid_plan =
  explain_json("SELECT * FROM test_strategy WHERE ctid = '(0,1)'");
my $tid_scan = find_plan_node($tid_plan, 'Tid Scan');
ok(defined $tid_scan, 'TID scan plan');
check_strategy($tid_scan, 'normal', 'TID scan');

my $tidrange_plan =
  explain_json(
	"SELECT * FROM test_strategy WHERE ctid >= '(0,1)' AND ctid < '(1,1)'");
my $tidrange_scan = find_plan_node($tidrange_plan, 'Tid Range Scan');
ok(defined $tidrange_scan, 'TID range scan plan');
check_strategy($tidrange_scan, 'normal', 'TID range scan');

my $worker_samplescan_plan = decode_json(
	$node->safe_psql(
		'postgres', qq{
SET max_parallel_workers_per_gather = 1;
SET debug_parallel_query = on;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  SELECT * FROM test_strategy TABLESAMPLE SYSTEM (100);
}))->[0]->{'Plan'};
check_worker_strategy($worker_samplescan_plan, 'Sample Scan', 'bulkread',
	'worker-only sample scan');

my $worker_tid_plan = decode_json(
	$node->safe_psql(
		'postgres', qq{
SET max_parallel_workers_per_gather = 1;
SET debug_parallel_query = on;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  SELECT * FROM test_strategy WHERE ctid = '(0,1)';
}))->[0]->{'Plan'};
check_worker_strategy($worker_tid_plan, 'Tid Scan', 'normal',
	'worker-only TID scan');

my $without_buffers = decode_json(
	$node->safe_psql(
		'postgres',
		'EXPLAIN (ANALYZE, BUFFERS OFF, VERBOSE, IO, FORMAT JSON) '
		  . 'SELECT * FROM test_strategy'))->[0]->{'Plan'};
ok( !exists $without_buffers->{'Buffer Access Strategies'},
	'strategy is not shown when BUFFERS is disabled');
ok( exists $without_buffers->{'Average Prefetch Distance'},
	'read stream statistics are shown when only IO is enabled');

my $without_verbose = decode_json(
	$node->safe_psql(
		'postgres',
		'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) '
		  . 'SELECT * FROM test_strategy'))->[0]->{'Plan'};
ok( !exists $without_verbose->{'Buffer Access Strategies'},
	'strategy is not shown when VERBOSE is disabled');

my $text = $node->safe_psql('postgres',
	'EXPLAIN (ANALYZE, BUFFERS, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) '
	  . 'SELECT * FROM test_strategy');
like(
	$text,
	qr/Buffer Access Strategies: bulkread/,
	'text output reports the strategy');

my $parallel_plan = decode_json(
	$node->safe_psql(
		'postgres', qq{
SET max_parallel_workers_per_gather = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET parallel_leader_participation = off;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  SELECT count(*) FROM test_strategy;
}))->[0]->{'Plan'};
my $gather = find_plan_node($parallel_plan, 'Gather');
my $parallel_scan = find_plan_node($parallel_plan, 'Seq Scan');

ok(defined $gather, 'parallel plan contains a Gather');
cmp_ok($gather->{'Workers Launched'}, '>', 0, 'parallel workers launched');
ok(defined $parallel_scan, 'parallel plan contains a sequential scan');
ok( !exists $parallel_scan->{'Buffer Access Strategies'},
	'strategy is not shown for a non-participating leader');
my $workers = $parallel_scan->{'Workers'};
ok(ref $workers eq 'ARRAY' && @$workers > 0,
	'parallel scan reports worker strategy information');

foreach my $worker (@$workers)
{
	is_deeply($worker->{'Buffer Access Strategies'},
		['bulkread'], 'parallel worker reports the strategy');
}

$node->stop;

done_testing();

# Run EXPLAIN for the query and return its top-level plan.
sub explain_json
{
	my ($query) = @_;
	my $output = $node->safe_psql('postgres',
		"EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON) $query");

	return decode_json($output)->[0]->{'Plan'};
}

# Check that a plan reports only the expected buffer access strategy.
sub check_strategy
{
	my ($plan, $strategy, $description) = @_;

	is_deeply($plan->{'Buffer Access Strategies'},
		[$strategy], "$description uses the $strategy strategy");
}

# Check that a worker-only scan reports only the expected strategy.
sub check_worker_strategy
{
	my ($plan, $type, $strategy, $description) = @_;
	my $scan = find_plan_node($plan, $type);

	ok(defined $scan, "$description plan contains a $type");
	ok( !exists $scan->{'Buffer Access Strategies'},
		"$description does not report a leader strategy");

	my $workers = $scan->{'Workers'};

	ok( ref $workers eq 'ARRAY' && @$workers > 0,
		"$description reports worker strategy information");
	foreach my $worker (@$workers)
	{
		is_deeply($worker->{'Buffer Access Strategies'},
			[$strategy], "$description worker uses the $strategy strategy");
	}
}

# Return the first plan node of the requested type, or undef if none exists.
sub find_plan_node
{
	my ($plan, $type) = @_;

	return $plan if $plan->{'Node Type'} eq $type;

	foreach my $child (@{ $plan->{'Plans'} // [] })
	{
		my $result = find_plan_node($child, $type);

		return $result if defined $result;
	}

	return;
}
