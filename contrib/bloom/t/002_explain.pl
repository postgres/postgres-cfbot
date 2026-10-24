# Copyright (c) 2026, PostgreSQL Global Development Group

# Test EXPLAIN reporting of buffer access strategies for index scans,
# including different access methods, early exits, and parallel execution.

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
CREATE TABLE btree_tst AS
  SELECT i, i AS payload FROM generate_series(1, 2000) AS i;
CREATE INDEX btreeidx ON btree_tst (i);
CREATE TABLE gin_tst AS
  SELECT ARRAY[i] AS a FROM generate_series(1, 2000) AS i;
CREATE INDEX ginidx ON gin_tst USING gin (a);
});

my $btree_plan = explain_btree_json();
my $btree_scan = find_plan_node($btree_plan, 'Index Scan');

ok(defined $btree_scan, 'plan contains a B-tree index scan');
is_deeply($btree_scan->{'Buffer Access Strategies'},
	['normal'], 'B-tree index scan uses the normal strategy');

my $btree_index_only_plan = explain_btree_index_only_json();
my $btree_index_only_scan =
  find_plan_node($btree_index_only_plan, 'Index Only Scan');

ok(defined $btree_index_only_scan, 'plan contains a B-tree index-only scan');
is_deeply($btree_index_only_scan->{'Buffer Access Strategies'},
	['normal'], 'B-tree index-only scan uses the normal strategy');

my $btree_null_plan = explain_btree_null_json();
my $btree_null_scan = find_plan_node($btree_null_plan, 'Index Scan');

ok(defined $btree_null_scan,
	'plan contains a B-tree index scan for a NULL parameter');
is($btree_null_scan->{'Index Searches'},
	0, 'B-tree exits before searching the index for a NULL parameter');
ok( !exists $btree_null_scan->{'Buffer Access Strategies'},
	'strategy is not shown when B-tree does not search the index');

my $never_executed_plan = explain_never_executed_gin_json();
my $never_executed_scan =
  find_never_executed_plan_node($never_executed_plan, 'Bitmap Index Scan');

ok(defined $never_executed_scan,
	'plan contains a never-executed GIN index scan');
ok( !exists $never_executed_scan->{'Buffer Access Strategies'},
	'strategy is not shown for a never-executed index scan');

my $plan = explain_json('BUFFERS');
my $heap_scan = find_plan_node($plan, 'Bitmap Heap Scan');
my $index_scan = find_plan_node($plan, 'Bitmap Index Scan');

ok(defined $heap_scan, 'plan contains a bitmap heap scan');
is_deeply($heap_scan->{'Buffer Access Strategies'},
	['normal'], 'bitmap heap scan uses the normal strategy');
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

my $without_buffers = explain_json('BUFFERS OFF');
$index_scan = find_plan_node($without_buffers, 'Bitmap Index Scan');

ok( defined $index_scan && !exists $index_scan->{'Buffer Access Strategies'},
	'strategy is not shown when BUFFERS is disabled');

$node->stop;

done_testing();

# Run EXPLAIN for a B-tree index scan and return the top-level plan.
sub explain_btree_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
  SELECT * FROM btree_tst WHERE i = 7;
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN for a B-tree index-only scan and return the top-level plan.
sub explain_btree_index_only_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
  SELECT i FROM btree_tst WHERE i = 7;
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN for a B-tree scan that exits before reading the index.
sub explain_btree_null_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET plan_cache_mode = force_generic_plan;
PREPARE btree_null_query(int) AS
  SELECT * FROM btree_tst WHERE i = \$1;
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
  EXECUTE btree_null_query(NULL);
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN with a never-executed gin index scan.
sub explain_never_executed_gin_json
{
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
  SELECT g.*
  FROM btree_tst b
  CROSS JOIN LATERAL
    (SELECT * FROM gin_tst g WHERE g.a @> ARRAY[b.i] OFFSET 0) g
  WHERE b.i = -1;
});

	return decode_json($output)->[0]->{'Plan'};
}

# Run EXPLAIN and return the top-level plan.
sub explain_json
{
	my ($buffers) = @_;
	my $output = $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off;
EXPLAIN (ANALYZE, $buffers, FORMAT JSON)
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
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
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
EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
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

# Find the first never-executed plan node of the requested type.
sub find_never_executed_plan_node
{
	my ($plan, $type) = @_;

	return $plan
	  if $plan->{'Node Type'} eq $type && $plan->{'Actual Loops'} == 0;

	foreach my $child (@{ $plan->{'Plans'} // [] })
	{
		my $result = find_never_executed_plan_node($child, $type);

		return $result if defined $result;
	}

	return;
}
