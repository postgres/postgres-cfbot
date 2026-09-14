# Copyright (c) 2026, PostgreSQL Global Development Group

# execute_undo_actions() applies a rollback in batches whose combined size is
# bounded by maintenance_work_mem: UndoRecordBulkFetch() stops a batch once the
# records it has read reach that many bytes.  The bound was computed as
#	int undo_apply_size = maintenance_work_mem * 1024L;
# which overflows a 32-bit int once maintenance_work_mem reaches 4 GB
# (maintenance_work_mem is PGC_USERSET up to MAX_KILOBYTES): 4 GB * 1024
# wraps to 0, and then UndoRecordBulkFetch() returns a single record per
# batch instead of the whole chain at once.  (At 2 GB the product is INT_MIN,
# which the Size comparison promotes to ~2^64, so the bound never trips and
# the whole chain is fetched unbounded -- that case needs >2 GB of undo to
# show and is not exercised here.)
#
# The injection point pbu-undo-apply-batch fires once per UndoRecordBulkFetch()
# call.  A small FLUX transaction applies its own undo inline (the default
# undo_rollback_overflow_size keeps it out of an undo apply worker), so a
# backend-local counter attached to that point and read back in the same
# backend counts the batches.  A rollback of many rows must take one batch.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_apply_batch_size');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}
# Where Size is 32 bits, maintenance_work_mem stops at MAX_KILOBYTES =
# INT_MAX / 1024, below the 4 GB this needs, and the product cannot overflow.
if ($node->safe_psql('postgres',
		"SELECT max_val::bigint < 4194304 FROM pg_settings WHERE name = 'maintenance_work_mem'")
	eq 't')
{
	plan skip_all => 'maintenance_work_mem cannot reach 4 GB on this platform';
}
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_idxundo;
CREATE TABLE t (k int PRIMARY KEY, v int) USING flux;
INSERT INTO t SELECT g, g FROM generate_series(1, 2000) g;
});

# Count the batches the inline rollback of a 2000-row UPDATE takes.  Each row
# changed writes an undo record, so with the bound collapsed to zero this is
# one batch per record (thousands); with the bound honored it is one.
my $batches = $node->safe_psql(
	'postgres', q{
SELECT injection_points_attach('pbu-undo-apply-batch', 'test_idxundo',
    'idxundo_count_batch', ''::bytea);
SET maintenance_work_mem = '4194304kB';   -- 4 GB: 4 GB * 1024 wraps int to 0
SELECT idxundo_reset_batch_count();
BEGIN; UPDATE t SET v = v + 1; ROLLBACK;
SELECT idxundo_batch_count();
});

note "batches = $batches";
cmp_ok($batches, '<=', 2,
	"rollback of 2000 rows takes one batch, not one per record (got $batches)");

$node->stop;
done_testing();
