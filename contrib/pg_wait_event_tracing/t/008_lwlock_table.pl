# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: the per-backend LWLock tranche table.
#
# Needs the assert-build-only test entry points compiled into the module
# (pwet_test_lwlock_table, pwet_test_struct_seq); the functions are
# declared here, never in the installed extension script, and the test is
# skipped on a build without assertions.
#
#   pwet_test_lwlock_table: with the table full, every admitted tranche is
#   still found however far it sits from its home slot (keys 1 + 32k all
#   collide in a 32-slot table of 16 entries), a tranche that was never
#   admitted is refused without changing occupancy, the cluster wraps
#   around the end of the array, and clear + refill works.
#
#   pwet_test_struct_seq: the structural sequence counter that makes a
#   cross-backend copy of the table consistent -- insertion, reset and
#   reinitialization leave it even and increased, an ordinary lookup hit
#   leaves it alone, and the reader rejects an odd value, a value that
#   changed across the copy, and a copy whose table is internally
#   inconsistent (dense_idx >= num_used, bad dimensions, wrong row count).

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'test entry points exist only in assert-enabled builds'
  unless check_pg_config('#define USE_ASSERT_CHECKING 1');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_wait_event_tracing'");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');

foreach my $fn (qw(pwet_test_lwlock_table pwet_test_struct_seq))
{
	$node->safe_psql('postgres',
		"CREATE FUNCTION $fn() RETURNS void "
		  . "AS 'pg_wait_event_tracing', '$fn' LANGUAGE C;");

	my ($ret, $stdout, $stderr) = $node->psql('postgres', "SELECT $fn();");
	is($ret, 0, "$fn passes") or diag $stderr;
}

$node->stop;
done_testing();
