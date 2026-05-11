
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test that lazy snapshot distribution reduces spill file usage.
#
# With the old eager distribution, each catalog-modifying commit (e.g. vacuum,
# DDL) would distribute a snapshot to every in-progress transaction in the
# reorder buffer.  When a long-running transaction coexists with many such
# commits, the snapshots accumulate with O(N^2) total size and spill to disk.
#
# With lazy distribution, snapshots are only distributed when a transaction
# actually decodes a data change, so a long-running transaction with few or
# no data changes receives at most one snapshot regardless of how many
# catalog-modifying commits happen.
#
# Note: invalidation messages are still eagerly distributed and they produce
# REORDER_BUFFER_CHANGE_INVALIDATION entries that count toward memory usage.
# We set logical_decoding_work_mem high enough to accommodate invalidation
# messages while verifying that snapshot distribution does not cause spilling.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', qq(
synchronous_commit = on
logical_decoding_work_mem = 1MB
autovacuum = off
));
$node->start;

# Setup
$node->safe_psql('postgres', qq[
    CREATE TABLE test_data (id int);
    SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');
]);

# Consume any setup-related changes
$node->safe_psql('postgres',
    "SELECT count(*) FROM pg_logical_slot_get_changes('test_slot', NULL, NULL)");

# Reset stats
$node->safe_psql('postgres',
    "SELECT pg_stat_reset_replication_slot('test_slot')");
$node->safe_psql('postgres', "SELECT pg_stat_force_next_flush()");

# Start a long-running transaction in a background session.
# This transaction will be in-progress while many catalog changes happen.
my $long_txn = $node->background_psql('postgres', on_error_stop => 1);
$long_txn->query_safe("BEGIN");
$long_txn->query_safe("INSERT INTO test_data VALUES (1)");

# In the main session, perform many catalog-modifying DDLs.
# Each DDL commit increments snapshot_generation and would have eagerly
# distributed a growing snapshot under the old code.
#
# With 200 DDLs (each CREATE TABLE is a catalog-modifying commit), the old
# eager approach would accumulate ~200 snapshots with O(N^2) total size in
# the long transaction's reorder buffer changes.
# With lazy distribution, only 1 snapshot is distributed when the long
# transaction does its next data change.
my $num_ddls = 200;
for my $i (1 .. $num_ddls) {
    $node->safe_psql('postgres', "CREATE TABLE dummy_$i (id int)");
}

# Insert one more row in the long transaction and commit.
$long_txn->query_safe("INSERT INTO test_data VALUES (2)");
$long_txn->query_safe("COMMIT");
$long_txn->quit;

# Consume the changes to trigger decoding
my $result = $node->safe_psql('postgres',
    "SELECT data FROM pg_logical_slot_get_changes('test_slot', NULL, NULL, " .
    "'include-xids', '0', 'skip-empty-xacts', '1')");

# Verify the decoded data is correct
like($result, qr/INSERT: id\[integer\]:1/, 'first INSERT decoded correctly');
like($result, qr/INSERT: id\[integer\]:2/, 'second INSERT decoded correctly');

# Check spill statistics.
# With lazy snapshot distribution and logical_decoding_work_mem=1MB, the long
# transaction should NOT spill.  It only contains 2 small INSERTs plus 1
# lazily-distributed snapshot, well under 1MB.
#
# Without the optimization, the long transaction would have accumulated ~200
# internal snapshots with a total size of ~O(N^2) bytes (~80KB for N=200),
# plus ~200 invalidation change entries.  The combined size could push the
# transaction over 1MB with larger N.
$node->safe_psql('postgres', "SELECT pg_stat_force_next_flush()");
$node->poll_query_until('postgres', qq[
    SELECT spill_bytes IS NOT NULL
    FROM pg_stat_replication_slots
    WHERE slot_name = 'test_slot'
]) or die "Timed out waiting for stats";

my $spill_bytes = $node->safe_psql('postgres', qq[
    SELECT spill_bytes
    FROM pg_stat_replication_slots
    WHERE slot_name = 'test_slot'
]);

is($spill_bytes, '0',
    "lazy snapshot distribution prevents spilling (spill_bytes=$spill_bytes)");

# Cleanup
for my $i (1 .. $num_ddls) {
    $node->safe_psql('postgres', "DROP TABLE IF EXISTS dummy_$i");
}
$node->safe_psql('postgres', qq[
    SELECT pg_drop_replication_slot('test_slot');
    DROP TABLE test_data;
]);

$node->stop;
done_testing();
