# Copyright (c) 2026, PostgreSQL Global Development Group

# An ATM entry whose UNDO chain cannot be applied (a record error, or a
# malformed chain) is retried up to LOGICAL_REVERT_MAX_RECORD_RETRIES times
# before the logical revert worker gives up on it.  The worker tracks the
# per-entry attempt count in a fixed, backend-local table of
# LOGICAL_REVERT_RETRY_SLOTS (64) slots.  revert_retry_bump() returns 0 when
# that table is full -- and the give-up guard "attempts > 0 && attempts < MAX"
# is then false, so an entry that could not get a slot was forgotten
# (ATMMarkReverted + ATMForget) on its VERY FIRST failing pass, releasing its
# WAL retention: a rollback faked as done that never ran.
#
# 65 ATM entries with malformed chains are planted.  On the first pass the
# first 64 take the retry slots and are retried; the 65th finds the table full.
# With a large naptime the first 64 are not due again, so after one pass the
# worker sleeps with the state settled: before the fix the 65th (and any
# beyond 64) have been forgotten, so fewer than 65 entries remain; with the fix
# a full table means "retry later" and all 65 are still queued.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $N = 65;    # one past the 64-slot retry table

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_retry_table_full');
$node->init;
# No worker yet (plant first): max_logical_revert_workers is PGC_POSTMASTER, so
# the launcher is absent at 0 and brought up by a restart below.  A long
# naptime makes a pass settle before the next one, so the count we read is the
# state after exactly one pass.
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 0
logical_revert_naptime = 30s
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

# Plant $N entries with malformed chains: each points at a checkpoint record,
# which can never be walked as an UNDO chain (as in 008_idxundo_revert_liveness).
# The xids wrote nothing, so treating them as aborted changes no data.
$node->safe_psql('postgres', 'CHECKPOINT');
for my $i (1 .. $N)
{
	my $xid = $node->safe_psql('postgres',
		'BEGIN; SELECT pg_current_xact_id(); COMMIT;');
	($xid) = $xid =~ /(\d+)\s*$/;
	$node->safe_psql('postgres',
		"SELECT idxundo_atm_add('$xid', checkpoint_lsn) FROM pg_control_checkpoint()"
	);
}
is($node->safe_psql('postgres', 'SELECT count(*) FROM idxundo_atm_entries()'),
	"$N", "all $N malformed entries queued");

# Bring up one worker and let it make a single pass, then settle (naptime is long).
my $log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 1');
$node->restart;
$node->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'logical revert worker'"
) or die "no revert worker started";

# The first pass is over once every entry has reported its failure: "will
# retry", or (before the fix, for the entry that found the table full) "giving
# up".  Count both, so the test neither samples too early nor waits forever.
# An entry with no retry slot is not held back by a retry delay, so a later
# pass may report it again: count at least one report per entry.
my $passed = 0;
for (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	my $log = substr(slurp_file($node->logfile), $log_offset);
	$passed = () =
	  $log =~ /UNDO chain of transaction \d+ is malformed at \S+, will retry|giving up on transaction \d+, whose UNDO chain is malformed/g;
	last if $passed >= $N;
	usleep(100_000);
}
cmp_ok($passed, '>=', $N, "the worker made one pass over all $N entries");
my $remaining =
  $node->safe_psql('postgres', 'SELECT count(*) FROM idxundo_atm_entries()');

note "remaining after one pass = $remaining of $N";
is($remaining, "$N",
	"a full retry table defers the overflow entry; none forgotten on the first failure");

# Nothing should have been given up on its first failure.
unlike(
	substr(slurp_file($node->logfile), $log_offset),
	qr/giving up on transaction \d+, whose UNDO chain is malformed/,
	'no entry given up as malformed after a single pass');

$node->stop;
done_testing();
