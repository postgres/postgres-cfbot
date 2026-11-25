# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the memory of query texts of pg_stat_statements, limited by
# pg_stat_statements.query_text_memory.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $limit = 2 * 1024 * 1024;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_stat_statements'
pg_stat_statements.max = 1000
pg_stat_statements.query_text_memory = 2MB
pg_stat_statements.track_utility = off
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');

# Execute distinct statements, which differ by their number of columns, with
# texts of about 50kB thanks to a comment, as nested statements of a DO block.
# Their texts need more than the query text memory, so the statements whose
# texts do not fit are not tracked.
$node->safe_psql(
	'postgres', q{
SET pg_stat_statements.track = 'all';
DO $$
BEGIN
  FOR n IN 1 .. 400 LOOP
    EXECUTE 'SELECT ' || array_to_string(array_fill(1, ARRAY[n]), ', ') ||
      ' /* ' || repeat('x', 50000) || ' */';
  END LOOP;
END
$$;
});

my ($count, $text_len, $dealloc, $memory) = split(
	/\|/,
	$node->safe_psql(
		'postgres', q{
SELECT count(*), coalesce(sum(length(query)), 0),
       (SELECT dealloc FROM pg_stat_statements_info),
       (SELECT query_text_memory FROM pg_stat_statements_info)
FROM pg_stat_statements}));

cmp_ok($count, '>', 0, 'statements tracked while their texts fit');
cmp_ok($count, '<', 400, 'statements whose texts do not fit not tracked');
is($dealloc, '0', 'no eviction for lack of query text memory');
cmp_ok($text_len, '<=', $limit, 'query texts bounded by the memory');
cmp_ok($memory, '<=', $limit, 'query text memory bounded');

# A statement whose text does not fit in the memory is not tracked.
my $too_long = "SELECT 'too long' AS too_long /* " . ('x' x 3000000) . " */";
my $too_long_query = q{
SELECT query IS NULL, calls FROM pg_stat_statements
WHERE toplevel AND (query IS NULL OR query LIKE 'SELECT $1 AS too_long%')};
$node->safe_psql('postgres', $too_long);
is($node->safe_psql('postgres', $too_long_query),
	'', 'statement not tracked without room for its text');

# It is tracked once the memory is raised.
$node->append_conf('postgresql.conf',
	'pg_stat_statements.query_text_memory = 8MB');
$node->reload;
$node->safe_psql('postgres', $too_long);
is($node->safe_psql('postgres', $too_long_query),
	'f|1', 'statement tracked after the memory is raised');

# Entries restored with a lowered memory keep their texts only up to it.
$node->append_conf('postgresql.conf',
	'pg_stat_statements.query_text_memory = 2MB');
$node->restart;
cmp_ok(
	$node->safe_psql(
		'postgres', 'SELECT query_text_memory FROM pg_stat_statements_info'),
	'<=', $limit,
	'query text memory bounded after restore');
is($node->safe_psql('postgres', $too_long_query),
	't|1', 'entry restored without its text');

# Texts are stored again once entries are removed.
$node->safe_psql('postgres', 'SELECT pg_stat_statements_reset()');
$node->safe_psql('postgres', 'SELECT 1 AS short_statement');
is( $node->safe_psql(
		'postgres',
		q{SELECT query FROM pg_stat_statements WHERE query LIKE '%short_statement'}
	),
	'SELECT $1 AS short_statement',
	'query text stored after a reset');

$node->stop;

done_testing();
