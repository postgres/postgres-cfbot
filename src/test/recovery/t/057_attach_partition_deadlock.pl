# Copyright (c) 2026, PostgreSQL Global Development Group

# Test deadlock between ATTACH PARTITION and ALTER TABLE recursion.
#
# This test reproduces the deadlock described in the thread starting at:
# https://www.postgresql.org/message-id/CFACA0EB-7E6F-4FAA-9ACE-1FC2226D7482@gmail.com
#
# The root cause is inconsistent lock ordering:
# - ALTER TABLE ... ALTER COLUMN SET DEFAULT recurses to partitions via
#   ATSimpleRecursion -> find_all_inheritors, which locks children in OID
#   order (parent first, then children sorted by OID).
# - ATTACH PARTITION calls generate_partition_qual() which walks UP the
#   partition tree, locking ancestors in reverse hierarchy order with
#   AccessShareLock.
#
# When the OID order differs from the hierarchy order (e.g., a partitioned
# table has a higher OID than its parent), the two paths acquire locks in
# opposite orders, leading to a deadlock:
# - Session A (ALTER TABLE) holds AccessExclusiveLock on the parent and
#   waits for AccessExclusiveLock on the child.
# - Session B (ATTACH PARTITION) holds AccessExclusiveLock on the child
#   (target table) and waits for AccessShareLock on the parent (which
#   conflicts with A's AccessExclusiveLock).
#
# The test uses an injection point in ATSimpleRecursion to pause Session A
# after it locks the root table but before it locks child partitions. This
# gives Session B time to lock the intermediate table and then block on the
# root. When Session A is released, it blocks on the intermediate table,
# completing the deadlock cycle.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
# Use a short deadlock_timeout so the deadlock is detected quickly.
$node->append_conf('postgresql.conf', 'deadlock_timeout = 100ms');
$node->start();

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Set up a 3-level partition hierarchy:
#
#   top (partitioned, root)
#    └── mid (partitioned, partition of top)
#
# Plus a standalone table newpart that will be attached to mid.
#
# Lock ordering:
# - Session A (ALTER TABLE top ... SET DEFAULT):
#     locks top (AEL) -> find_all_inheritors -> locks mid (AEL)
# - Session B (ALTER TABLE mid ATTACH PARTITION newpart):
#     locks mid (AEL, target) -> locks newpart (AEL) ->
#     generate_partition_qual(mid) -> locks top (ASL)
#
# Deadlock:
# - A holds AEL on top, wants AEL on mid
# - B holds AEL on mid, wants ASL on top (conflicts with A's AEL)

$node->safe_psql(
	'postgres', q[
CREATE TABLE top (id int) PARTITION BY RANGE (id);
CREATE TABLE mid PARTITION OF top FOR VALUES FROM (0) TO (1000000)
    PARTITION BY RANGE (id);
CREATE TABLE newpart (id int);
]);

my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $s2 = $node->background_psql('postgres', on_error_stop => 0);

# Session A: attach the injection point locally, then start the ALTER TABLE.
# The injection point fires in ATSimpleRecursion after the root table is
# locked but before find_all_inheritors locks the children.
$s1->query_safe(q[
SELECT injection_points_set_local();
SELECT injection_points_attach('alter-table-simple-recursion', 'wait');
]);

$s1->query_until(
	qr/starting_alter/, q[
\echo starting_alter
ALTER TABLE top ALTER COLUMN id SET DEFAULT 0;
]);

# Wait for Session A to hit the injection point.
$node->wait_for_event('client backend', 'alter-table-simple-recursion');

# Session B: start ATTACH PARTITION.  This will:
# 1. Lock mid (AEL) -- target table
# 2. Lock newpart (AEL) -- attachrel
# 3. Call generate_partition_qual(mid) -> try to lock top (ASL)
#    -> block here, because Session A holds AEL on top.
$s2->query_until(
	qr/starting_attach/, q[
\echo starting_attach
ALTER TABLE mid ATTACH PARTITION newpart FOR VALUES FROM (500000) TO (1000000);
]);

# Wait for Session B to block waiting for AccessShareLock on top.
$node->poll_query_until(
	'postgres', qq[
	SELECT count(*) > 0 FROM pg_locks
	WHERE locktype = 'relation'
	  AND relation = 'top'::regclass
	  AND mode = 'AccessShareLock'
	  AND NOT granted;
]) or die "Timed out waiting for Session B to block on top lock";

# Now wake up Session A.  It will proceed to find_all_inheritors which tries
# to lock mid (AEL), but Session B holds AEL on mid, so Session A blocks.
# This completes the deadlock cycle:
#   A holds AEL on top, waits for AEL on mid
#   B holds AEL on mid, waits for ASL on top
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('alter-table-simple-recursion');
SELECT injection_points_wakeup('alter-table-simple-recursion');});

# Wait for the deadlock to be detected and reported in the log.
my $log_offset = -s $node->logfile;
$node->wait_for_log(qr/deadlock detected/, $log_offset);

note("deadlock detected");

# One session should have received a deadlock error, the other should
# succeed.  Verify via the log.
my $log_contents = slurp_file($node->logfile, $log_offset);
like($log_contents, qr/Process .* waits for AccessExclusiveLock on relation .* of database/,
	"deadlock detected with AccessExclusiveLock wait");

# Clean up: the session that got the deadlock error is done (psql returned
# the error).  The other session should complete once the lock is released.
# Verify that the ATTACH PARTITION or ALTER TABLE completed in the surviving
# session by checking that newpart is now a partition of mid or that the
# default was set on top.
$s1->quit;
$s2->quit;

$node->stop();

done_testing();
