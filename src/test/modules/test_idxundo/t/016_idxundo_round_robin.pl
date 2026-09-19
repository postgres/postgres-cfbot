# Copyright (c) 2026, PostgreSQL Global Development Group

# GetRollbackHashKeyFromQueue() must not advance the round-robin
# cursor itself: UndoGetWork()'s round robin advances it once per iteration,
# so an extra advance on an empty ERROR_QUEUE double-steps and skips
# XID_QUEUE.  With one servable request in XID_QUEUE and the cursor started at
# the empty ERROR_QUEUE, the three-iteration scan must still reach XID_QUEUE
# and find the request.
#
# Before the fix the empty-ERROR_QUEUE branch did cur_undo_queue++ of its own,
# and the caller advanced again, so the scan stepped ERROR -> SIZE -> ERROR
# and never looked at XID_QUEUE: UndoGetWork returned "no work" with work
# waiting.  idxundo_round_robin_finds_xid() reports whether the request was
# found (needs an injection-points build for the test probe).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_round_robin');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

is($node->safe_psql('postgres', 'SELECT idxundo_round_robin_finds_xid()'),
	't',
	'the round robin reaches XID_QUEUE past an empty ERROR_QUEUE');

$node->stop;
done_testing();
