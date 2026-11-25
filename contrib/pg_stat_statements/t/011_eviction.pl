# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the eviction of pg_stat_statements entries, and for changes of
# pg_stat_statements.max with a reload and across restarts.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_stat_statements'
pg_stat_statements.max = 200
pg_stat_statements.track_utility = off
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');

# Run "count" distinct statements, which differ by their number of columns,
# starting from "first".
sub run_distinct
{
	my ($first, $count) = @_;
	my $sql = '';

	for my $n ($first .. $first + $count - 1)
	{
		$sql .= 'SELECT ' . join(', ', (1) x $n) . ";\n";
	}
	$node->safe_psql('postgres', $sql);
}

my $count_query = "SELECT count(*) FROM pg_stat_statements";

# A backend running a statement again in the same transaction, after its
# entry was removed by another backend, finds its reference to the removed
# entry stale, discards its pending statistics and counts in a new entry.
my $session = $node->background_psql('postgres');
$session->query_safe("BEGIN");
$session->query_safe("SELECT count(*) AS stale FROM pg_class");
$node->safe_psql('postgres', "SELECT pg_stat_statements_reset()");
$session->query_safe("SELECT count(*) AS stale FROM pg_class");
$session->query_safe("COMMIT");
$session->query_safe("SELECT pg_stat_force_next_flush()");
$session->quit;
is( $node->safe_psql(
		'postgres',
		"SELECT calls, query <> '' FROM pg_stat_statements WHERE query LIKE '%AS stale FROM pg_class'"
	),
	'1|t',
	'statement counted in a new entry after its entry was removed');

# A statement executed often, which eviction keeps.
$node->safe_psql('postgres',
	"SELECT 'hot' AS hot;\n" x 50 . "SELECT pg_stat_force_next_flush();");

run_distinct(1, 400);

my $count = $node->safe_psql('postgres', $count_query);
cmp_ok($count, '<=', 200, 'number of entries bounded by .max');
cmp_ok($count, '>=', 150, 'eviction keeps most entries');
cmp_ok($node->safe_psql('postgres',
		"SELECT dealloc FROM pg_stat_statements_info"),
	'>', 0, 'deallocations counted');
is( $node->safe_psql(
		'postgres',
		"SELECT calls FROM pg_stat_statements WHERE query LIKE '%AS hot'"),
	'50',
	'frequently executed statement kept');

# Lowering .max with a reload applies at the next new entry, and trims to
# the new maximum, not further.
$node->append_conf('postgresql.conf', 'pg_stat_statements.max = 100');
$node->reload;
run_distinct(401, 1);

$count = $node->safe_psql('postgres', $count_query);
cmp_ok($count, '<=', 100, 'number of entries bounded by lowered .max');
cmp_ok($count, '>=', 80, 'lowering .max keeps most entries');

# Lowering .max across a restart trims the restored entries.
$node->append_conf('postgresql.conf', 'pg_stat_statements.max = 200');
$node->reload;
run_distinct(500, 200);
$node->stop;
$node->append_conf('postgresql.conf', 'pg_stat_statements.max = 100');
$node->start;

$count = $node->safe_psql('postgres', $count_query);
cmp_ok($count, '<=', 100, 'restored entries trimmed to lowered .max');
cmp_ok($count, '>=', 80, 'trimming restored entries keeps most of them');

$node->stop;

done_testing();
