# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# Tests for checking that pg_stat_statements contents are preserved
# across restarts.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_stat_statements'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');

$node->safe_psql('postgres', 'CREATE TABLE t1 (a int)');
$node->safe_psql('postgres', 'SELECT a FROM t1');

is( $node->safe_psql(
		'postgres',
		"SELECT query FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%' ORDER BY query"
	),
	"CREATE TABLE t1 (a int)\nSELECT a FROM t1",
	'pg_stat_statements populated');

my $count_query =
  "SELECT count(*) FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%'";
my $stats_reset_query =
  "SELECT stats_reset::text FROM pg_stat_statements_info";

my $stats_reset = $node->safe_psql('postgres', $stats_reset_query);

$node->restart;

is( $node->safe_psql(
		'postgres',
		"SELECT query FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%' ORDER BY query"
	),
	"CREATE TABLE t1 (a int)\nSELECT a FROM t1",
	'pg_stat_statements data kept across restart');

is( $node->safe_psql(
		'postgres',
		"SELECT calls FROM pg_stat_statements WHERE query = 'SELECT a FROM t1'"
	),
	'1',
	'pg_stat_statements counters kept across restart');
is($node->safe_psql('postgres', $stats_reset_query),
	$stats_reset, 'pg_stat_statements_info kept across restart');

$node->append_conf('postgresql.conf', "pg_stat_statements.save = false");
$node->reload;

$node->restart;

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%'"
	),
	'0',
	'pg_stat_statements data not kept across restart with .save=false');

# Entries are not restored if pg_stat_statements.save is off at startup
# only, and neither are global statistics.
$node->append_conf('postgresql.conf', "pg_stat_statements.save = true");
$node->reload;
$node->safe_psql('postgres', 'SELECT a FROM t1');
$stats_reset = $node->safe_psql('postgres', $stats_reset_query);
$node->stop;
$node->append_conf('postgresql.conf', "pg_stat_statements.save = false");
$node->start;

is($node->safe_psql('postgres', $count_query),
	'0', 'pg_stat_statements data not restored with .save=false at startup');
isnt($node->safe_psql('postgres', $stats_reset_query),
	$stats_reset,
	'pg_stat_statements_info not restored with .save=false at startup');

# Entries are saved without their texts if pg_stat_statements.save is off
# at shutdown, and are not restored then.
$node->safe_psql('postgres', 'SELECT a FROM t1');
$node->stop;
$node->append_conf('postgresql.conf', "pg_stat_statements.save = true");
$node->start;

is($node->safe_psql('postgres', $count_query),
	'0',
	'pg_stat_statements data not restored with .save=false at shutdown');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_statements WHERE query IS NULL"),
	'0',
	'pg_stat_statements entries without texts not restored');

# Statistics are discarded after a crash.
$node->safe_psql('postgres', 'SELECT a FROM t1');
$stats_reset = $node->safe_psql('postgres', $stats_reset_query);
$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', $count_query),
	'0', 'pg_stat_statements data discarded after a crash');
isnt($node->safe_psql('postgres', $stats_reset_query),
	$stats_reset, 'pg_stat_statements_info reset after a crash');

# Statistics are discarded with a corrupted statistics file, and new entries
# still work.
$node->safe_psql('postgres', 'SELECT a FROM t1');
$node->stop;
my $statsfile = $node->data_dir . '/pg_stat/pgstat.stat';
truncate($statsfile, int((-s $statsfile) / 2))
  or die "could not truncate \"$statsfile\": $!";
$node->start;

is($node->safe_psql('postgres', $count_query),
	'0', 'pg_stat_statements data discarded with a corrupted stats file');
$node->safe_psql('postgres', 'SELECT a FROM t1');
is( $node->safe_psql(
		'postgres',
		"SELECT query FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%' ORDER BY query"
	),
	'SELECT a FROM t1',
	'pg_stat_statements works after a corrupted stats file');

$node->stop;

done_testing();
