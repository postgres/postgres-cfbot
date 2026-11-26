# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Basic test for register_sync_handler() dispatch.
#
# Verifies that a custom sync handler registered via register_sync_handler()
# in _PG_init() receives callback invocations from ProcessSyncRequests() at
# CHECKPOINT time, that identical FileTags coalesce via HASH_BLOBS
# deduplication, that distinct FileTags produce distinct callbacks, and
# that an idle checkpoint does not re-dispatch entries that were already
# processed (cycle_ctr skip).  Also checks that unlink and filter requests
# for a handler without those callbacks are refused without a crash.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('sync_handler');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_sync_handler'
# TAP clusters set fsync = off by default for speed; re-enable here so
# that ProcessSyncRequests actually dispatches our sync handler callback.
fsync = on
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_sync_handler');

# The handler ID must be >= SYNC_HANDLER_FIRST_DYNAMIC. Built-ins
# currently occupy IDs 0..4, so the first extension handler should be
# at least 5.
my $id = $node->safe_psql('postgres', 'SELECT test_sync_handler_id()');
ok($id >= 5,
	"handler id $id is >= SYNC_HANDLER_FIRST_DYNAMIC (built-ins = 5)")
  or diag("got id=$id");

# Baseline: no dispatches before we queue anything.
my $baseline =
  $node->safe_psql('postgres', 'SELECT test_sync_handler_count()');
is($baseline, '0', 'baseline dispatch count is zero');

# Queue 5 distinct FileTags (differing in segno only) and checkpoint.
# Expect 5 callback invocations since they are all distinct hash keys.
$node->safe_psql(
	'postgres', q{
SELECT test_sync_handler_register(1);
SELECT test_sync_handler_register(2);
SELECT test_sync_handler_register(3);
SELECT test_sync_handler_register(4);
SELECT test_sync_handler_register(5);
});
$node->safe_psql('postgres', 'CHECKPOINT');
my $after_distinct =
  $node->safe_psql('postgres', 'SELECT test_sync_handler_count()');
is($after_distinct, '5',
	'5 distinct FileTags produce 5 sync_syncfiletag callbacks')
  or diag("got $after_distinct");

# Queue 10 duplicate FileTags (same segno 42) and checkpoint.
# Expect exactly 1 additional callback because pendingOps uses HASH_BLOBS
# and collapses identical FileTags into a single hash entry.
$node->safe_psql(
	'postgres', q{
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
SELECT test_sync_handler_register(42);
});
$node->safe_psql('postgres', 'CHECKPOINT');
my $after_coalesce =
  $node->safe_psql('postgres', 'SELECT test_sync_handler_count()');
is($after_coalesce, '6',
	'10 duplicate FileTags coalesce via HASH_BLOBS to 1 additional callback')
  or diag("got $after_coalesce");

# Second CHECKPOINT with no new requests. The count must stay the same:
# every entry from the previous checkpoint was processed and removed
# from pendingOps, and no new entries have been queued, so
# ProcessSyncRequests has nothing to dispatch.
$node->safe_psql('postgres', 'CHECKPOINT');
my $after_idle =
  $node->safe_psql('postgres', 'SELECT test_sync_handler_count()');
is($after_idle, '6', 'idle checkpoint does not re-dispatch')
  or diag("got $after_idle");

# Unlink and filter requests for a handler that has both callbacks are
# accepted, and the following checkpoint completes.
$node->safe_psql(
	'postgres', q{
SELECT test_sync_handler_request('unlink', false, false);
SELECT test_sync_handler_request('filter', false, false);
});
$node->safe_psql('postgres', 'CHECKPOINT');
pass('unlink and filter requests for a handler with both callbacks');

# A handler with only sync_syncfiletag, like the built-in CLOG handler.
# RegisterSyncRequest() must refuse unlink and filter requests for it in the
# calling backend.  Before, an unlink request crashed the checkpointer when
# it called the NULL sync_unlinkfiletag.
my $checkpointer_pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'");

foreach my $kind ('unlink', 'filter')
{
	my $callback =
	  $kind eq 'unlink' ? 'sync_unlinkfiletag' : 'sync_filetagmatches';
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		"SELECT test_sync_handler_request('$kind', true, false)");
	isnt($ret, 0, "$kind request for a handler without $callback fails");
	like(
		$stderr,
		qr/cannot queue an? $kind request for sync handler "test_sync_handler_nocb", which has no $callback callback/,
		"$kind request names the handler and the missing callback");
}

# The same requests forwarded straight to the checkpointer skip that check.
# The checkpointer must drop them with a warning, not crash and not error out
# (an error there would discard the other requests it was absorbing).
my $log_offset = -s $node->logfile;
$node->safe_psql(
	'postgres', q{
SELECT test_sync_handler_request('unlink', true, true);
SELECT test_sync_handler_request('filter', true, true);
});
$node->safe_psql('postgres', 'CHECKPOINT');
foreach my $kind ('unlink', 'filter')
{
	my $callback =
	  $kind eq 'unlink' ? 'sync_unlinkfiletag' : 'sync_filetagmatches';
	ok( $node->log_contains(
			qr/WARNING:  ignoring $kind request for sync handler "test_sync_handler_nocb", which has no $callback callback/,
			$log_offset),
		"forwarded $kind request is dropped with a warning");
}

is( $node->safe_psql(
		'postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'"),
	$checkpointer_pid,
	'checkpointer was not restarted, so no process crashed');
$node->safe_psql('postgres', 'CHECKPOINT');
pass('checkpoint still completes');

$node->stop;

done_testing();
