# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: memory footprint bound (fix 1).
#
# The statistics-level collector's shared-memory footprint must stay
# sparse: the always-resident control segment holds only one small
# PwetSlot per possible backend, and the ~208 KiB-per-backend timing
# payload is allocated from a DSA area only for a backend that actually
# enables capture, not for every backend up front (v6's dense design
# would need roughly 238 slots * ~206 KiB =~ 48 MiB at this test's
# max_connections).  This test measures the module's DSM registry
# footprint -- the control segment "pg_wait_event_tracing" plus the DSA
# area "pg_wait_event_tracing_stats" -- across a sequence of sessions
# enabling and disabling capture, and checks it stays within the
# sparse-design bounds instead of scaling with max_connections.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_wait_event_tracing'");
$node->append_conf('postgresql.conf', "max_connections = 200");
# Keep pg_sleep() running in the session that issued it, not a parallel
# worker, so its wait is recorded under the pid this test is watching.
$node->append_conf('postgresql.conf', "debug_parallel_query = off");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_wait_event_tracing;');

# Sum of the DSM registry entries this module owns: the small,
# always-resident control segment plus the DSA area backing per-backend
# payloads.  dsa_get_total_size_from_handle() sums all of a DSA's
# segments, so a single row already reflects the whole area.
sub pwet_footprint
{
	return $node->safe_psql(
		'postgres',
		"SELECT coalesce(sum(size), 0) FROM pg_dsm_registry_allocations "
		  . "WHERE name LIKE 'pg_wait_event_tracing%';");
}

my $KiB = 1024;
my $MiB = 1024 * $KiB;

my $baseline = pwet_footprint();
is($baseline, '0',
	"no pg_wait_event_tracing shared memory before any backend captures");

# One session enables stats and records a single wait.  This is what
# creates the control segment and the stats DSA area in the first
# place.
my $s1 = $node->background_psql('postgres');
$s1->query_safe("SET pg_wait_event_tracing.capture = stats;");
$s1->query_safe("SELECT pg_sleep(0.01);");
my $s1_pid = $s1->query_safe("SELECT pg_backend_pid();");

my $after_s1 = pwet_footprint();
cmp_ok($after_s1 - $baseline, '<', 4 * $MiB,
	"one capturing backend's footprint stays well under the dense "
	  . "design's per-max_connections bound");

# A second session enabling stats should only need its own payload
# (already sized well under 512 KiB), not another whole DSA segment on
# top of the first.
my $s2 = $node->background_psql('postgres');
$s2->query_safe("SET pg_wait_event_tracing.capture = stats;");
# Attach through the next statement's parse analysis, so this
# measurement does not depend on whether the SET itself attached,
# and confirm the session really is collecting before measuring.
$s2->query_safe("SELECT pg_sleep(0.01);");
my $s2_pid = $s2->query_safe("SELECT pg_backend_pid();");
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_wait_event_timing "
		  . "WHERE pid = $s2_pid;"),
	't',
	"session s2 is collecting before its footprint is measured");

my $after_s2 = pwet_footprint();
cmp_ok($after_s2 - $after_s1, '<', 512 * $KiB,
	"a second capturing backend adds only its own payload, not another "
	  . "DSA segment");

# Turning capture off releases the first backend's payload; its pid
# must disappear from the view even though nothing else about the
# backend changed (fix 2's ownership check, exercised here via fix 1's
# release path).
$s1->query_safe("SET pg_wait_event_tracing.capture = off;");
my $s1_rows = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_wait_event_timing WHERE pid = $s1_pid;");
is($s1_rows, '0',
	"disabling capture removes the backend's rows from the timing view");

# A third session enabling stats should reuse the freed payload rather
# than growing the DSA area again.
my $s3 = $node->background_psql('postgres');
$s3->query_safe("SET pg_wait_event_tracing.capture = stats;");
# Attach through the next statement's parse analysis, so this
# measurement does not depend on whether the SET itself attached,
# and confirm the session really is collecting before measuring.
$s3->query_safe("SELECT pg_sleep(0.01);");
my $s3_pid = $s3->query_safe("SELECT pg_backend_pid();");
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_wait_event_timing "
		  . "WHERE pid = $s3_pid;"),
	't',
	"session s3 is collecting before its footprint is measured");

my $after_s3 = pwet_footprint();
cmp_ok($after_s3, '<=', $after_s2,
	"a third capturing backend reuses the freed payload instead of "
	  . "growing the footprint");

$s1->quit;
$s2->quit;
$s3->quit;

done_testing();
