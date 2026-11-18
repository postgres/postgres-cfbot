# Copyright (c) 2026, PostgreSQL Global Development Group

# AtAbort_XactUndo()'s inline-abort path must restore the memory context and
# free its working context on every exit, including the early exit taken when
# the batch LSN does not point at a valid UNDO batch.
#
# That exit jumped over the MemoryContextSwitchTo(old_ctx) /
# MemoryContextDelete(undo_ctx) pair, so the "Inline UNDO Apply" context (a
# child of TopMemoryContext) leaked on every such abort, and the rest of the
# abort ran in, and returned into, that context.  One leaked context is
# enough to show it: it survives into the next statement of the same
# backend, where pg_backend_memory_contexts lists it.
#
# The invalid-batch exit is forced with the "inline-undo-invalid-batch"
# injection point.  A FILEOPS chmod puts one UNDO record in the
# transaction's chain so the inline path runs at all.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_inline_abort_ctx');
$node->init;
# FILEOPS writes UNDO only when WAL is needed.
$node->append_conf('postgresql.conf', 'wal_level = replica');
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_fileops; CREATE EXTENSION injection_points;');

my $path = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('inline_abort_ctx.dat')");

# One backend: the leaked context has to be looked for in the backend that
# aborted, after its transaction has ended.
my $bg = $node->background_psql('postgres');
$bg->query_safe('SELECT injection_points_set_local()');
$bg->query_safe(
	"SELECT injection_points_attach('inline-undo-invalid-batch', 'error')");

$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_chmod('$path', 384)");
$bg->query('ROLLBACK');

# The ROLLBACK warns "inline UNDO failed"; query() leaves that on stderr.
my $leaked = $bg->query(
	"SELECT count(*) FROM pg_backend_memory_contexts "
	  . "WHERE name = 'Inline UNDO Apply'");
chomp $leaked;
is($leaked, '0',
	'the inline-abort working context is freed on the invalid-batch exit');

$bg->quit;
$node->stop;
done_testing();
