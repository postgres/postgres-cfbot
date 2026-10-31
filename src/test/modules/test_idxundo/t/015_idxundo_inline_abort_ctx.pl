# Copyright (c) 2026, PostgreSQL Global Development Group

# AtAbort_XactUndo()'s inline-abort path must restore the memory context and
# free its working context on every exit, including the early "defer to ATM"
# path taken when the batch LSN does not point at a valid UNDO batch.
#
# That path reached its goto by jumping *over* the
# MemoryContextSwitchTo(old_ctx) / MemoryContextDelete(undo_ctx) pair, so the
# "Inline UNDO Apply" context (a child of TopMemoryContext) leaked on every
# failed inline abort and the rest of the abort routine ran in, and returned
# in, that soon-to-dangle context.  One leaked context is enough to show it:
# it survives into the next statement of the same backend, where
# pg_backend_memory_contexts lists it.
#
# The stale-LSN window is forced deterministically with the
# "inline-undo-invalid-batch" injection point.  idxundo_write_failing_undo()
# puts one small (inline-abortable) UNDO record in the cluster-wide engine's
# chain so the inline path runs at all.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_inline_abort_ctx');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_logical_revert_workers = 0
undo_instant_abort_threshold = 65536
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE EXTENSION injection_points;
});

# One persistent backend: the leaked context has to be observed in the same
# backend that aborted, after the aborting transaction has ended.
my $bg = $node->background_psql('postgres');
$bg->query_safe(
	"SELECT injection_points_attach('inline-undo-invalid-batch', 'error')");

# A small inline-abortable transaction whose inline apply is forced down the
# "invalid batch -> defer to ATM" path.
$bg->query_safe('BEGIN');
$bg->query_safe('SELECT idxundo_write_failing_undo()');
$bg->query('ROLLBACK');

my $leaked = $bg->query_safe(
	"SELECT count(*) FROM pg_backend_memory_contexts "
	  . "WHERE name = 'Inline UNDO Apply'");
is($leaked, '0',
	'the inline-abort working context is freed on the defer-to-ATM path');

$bg->quit;
$node->stop;
done_testing();
