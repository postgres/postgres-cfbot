# Copyright (c) 2026, PostgreSQL Global Development Group

# Test EXPLAIN reporting of buffer access strategies for Bloom index scans,
# including early exits and parallel execution.

use strict;
use warnings FATAL => 'all';

use JSON::PP qw(decode_json);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Keep the test table below the bulk-read threshold
$node->append_conf('postgresql.conf', 'shared_buffers = 1MB');
$node->start;

$node->safe_psql(
	'postgres', qq{
CREATE EXTENSION bloom;
CREATE TABLE tst AS SELECT i % 10 AS i FROM generate_series(1, 2000) AS i;
CREATE INDEX bloomidx ON tst USING bloom (i);
});

my $plan = explain_json('BUFFERS, VERBOSE');
my $index_scan = find_plan_node($plan, 'Bitmap Index Scan');

ok(defined $index_scan, 'plan contains a bitmap index scan');
is_deeply($index_scan->{'Buffer Access Strategies'},
	['bulkread'], 'bloom index scan uses the bulkread strategy');

my $null_plan = explain_bloom_null_json();
my $null_index_scan = find_plan_node($null_plan, 'Bitmap Index Scan');

ok(defined $null_index_scan,
	'plan contains a Bloom bitmap index scan for a NULL parameter');
is($null_index_scan->{'Index Searches'},
	0, 'Bloom exits before searching the index for a NULL parameter');
ok( !exists $null_index_scan->{'Buffer Access Strategies'},
	'strategy is not shown when Bloom does not search the index');

my $parallel_plan = explain_parallel_json();
my $gather = find_plan_node($parallel_plan, 'Gather');
my $parallel_index_scan = find_plan_node($parallel_plan, 'Bitmap Index Scan');

ok(defined $gather, 'parallel plan contains a Gather');
cmp_ok(defined $gather ? $gather->{'Workers Launched'} : 0,
	'>', 0, 'parallel worker launched');
ok(defined $parallel_index_scan,
	'parallel plan contains a bitmap index scan');
ok( !exists $parallel_index_scan->{'Buffer Access Strategies'},
	'strategy is not shown for the non-participating leader');
my $workers = $parallel_index_scan->{'Workers'};
ok(ref $workers eq 'ARRAY' && @$workers > 0,
	'parallel bitmap index scan reports worker information');

foreach my $worker (@$workers)
{
	is_deeply($worker->{'Buffer Access Strategies'},
		['bulkread'], 'parallel worker uses the bulkread strategy');
}

my $without_buffers = explain_json('BUFFERS OFF, VERBOSE');
$index_scan = find_plan_node($without_buffers, 'Bitmap Index Scan');

ok( defined $index_scan && !exists $index_scan->{'Buffer Access Strategies'},
	'strategy is not shown when BUFFERS is disabled');

my $without_verbose = explain_json('BUFFERS');
$index_scan = find_plan_node($without_verbose, 'Bitmap Index Scan');

ok( defined $index_scan && !exists $index_scan->{'Buffer Access Strategies'},
	'strategy is not shown when VERBOSE is disabled');

$node->stop;

done_testing();

# Run EXPLAIN and return the top-level plan.
sub explain_json
{
	my ($options) = @_;
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
EXPLAIN (ANALYZE, $options, FORMAT JSON)
  SELECT * FROM tst WHERE i = 7;
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN for a Bloom scan that exits before reading the index.
sub explain_bloom_null_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
SET plan_cache_mode = force_generic_plan;
PREPARE bloom_null_query(int) AS SELECT * FROM tst WHERE i = \$1;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  EXECUTE bloom_null_query(NULL);
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN in a parallel worker and return the top-level plan.
sub explain_parallel_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET max_parallel_workers_per_gather = 1;
SET debug_parallel_query = on;
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON)
  SELECT * FROM tst WHERE i = 7;
});

	return decode_json($output)->[0]->{'Plan'};
}

# Find the first plan node of the requested type.
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
