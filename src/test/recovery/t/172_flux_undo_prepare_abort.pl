# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A FLUX DELETE and UPDATE call FluxPbuPrepareInsert() to reserve their UNDO
# record before the critical section: that sets flux_pbu_prepared and pins the
# per-backend UNDO engine's undo buffers.  If a step between that reservation
# and START_CRIT_SECTION can ereport(ERROR) -- FluxVMPinForChange() may, on I/O
# reading the visibility-map page -- the reservation is stranded when the
# (sub)transaction aborts: the engine's end-of-xact undo processing does not
# touch a prepared-but-not-inserted record, so the next DML in the same backend
# trips Assert(!flux_pbu_prepared) in FluxPbuPrepareInsert (a cassert PANIC), or
# in a non-assert build drives PrepareUndoInsert with stale engine buffer state.
#
# The fix reorders the DELETE/UPDATE so every error-able pre-crit step (the XID
# assignment and FluxVMPinForChange) runs BEFORE FluxPbuPrepareInsert, leaving
# nothing error-able between the reservation and START_CRIT_SECTION.  The
# injection point flux-delete-after-undo-prepare models the former
# FluxVMPinForChange error; on the fixed code it fires before the reservation
# exists, so the error strands nothing and later DML on the same backend works.
# (Pre-fix the point sat after the reservation and the next DML PANICed.)
#
# Both an aborted subtransaction (resumed after ROLLBACK TO) and an aborted
# top-level transaction (followed by a fresh transaction) are covered.
#
# Each statement is issued on one interactive session so the one that errors and
# the ones after it run on the same backend; a single ON_ERROR_STOP script would
# abort at the first error and never exercise the next DML.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_undo_prepare_abort');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE f (id int, v text) USING flux;
INSERT INTO f SELECT g, 'x' || g FROM generate_series(1, 20) g;
});

my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe("SELECT injection_points_set_local()");

# Case 1: subtransaction.  The DELETE errors in the (former) window inside a
# savepoint; after ROLLBACK TO the same backend keeps running on the same
# connection and later DML must not assert.
$s->query_safe(
	"SELECT injection_points_attach('flux-delete-after-undo-prepare', 'error')");
$s->query_safe('BEGIN');
$s->query_safe('SAVEPOINT s1');
my ($out, $ret) = $s->query('DELETE FROM f WHERE id = 1');
like($s->{stderr},
	qr/error triggered for injection point flux-delete-after-undo-prepare/,
	'the DELETE errored in the prepare->crit window (subxact)');
$s->{stderr} = '';    # consume the expected error
$s->query_safe('ROLLBACK TO SAVEPOINT s1');
$s->query_safe("SELECT injection_points_detach('flux-delete-after-undo-prepare')");

# Before the fix the first of these tripped Assert(!flux_pbu_prepared) with the
# undo record the rolled-back DELETE left prepared.  A second savepoint cycle
# exercises the path more than once.
$s->query_safe('UPDATE f SET v = v || \'u\' WHERE id = 2');
$s->query_safe('SAVEPOINT s2');
$s->query_safe('DELETE FROM f WHERE id = 3');
$s->query_safe('RELEASE SAVEPOINT s2');
$s->query_safe('COMMIT');
pass('DML after the prepare->crit error and subxact rollback succeeded');

# Case 2: top-level.  The DELETE errors in the (former) window in an implicit
# transaction, aborting it; a fresh transaction on the same connection must not
# assert (the engine does not reset a prepared-but-not-inserted record at abort,
# so pre-fix this also stranded flux_pbu_prepared).
$s->query_safe(
	"SELECT injection_points_attach('flux-delete-after-undo-prepare', 'error')");
($out, $ret) = $s->query('DELETE FROM f WHERE id = 4');
like($s->{stderr},
	qr/error triggered for injection point flux-delete-after-undo-prepare/,
	'the DELETE errored in the prepare->crit window (top-level)');
$s->{stderr} = '';    # consume the expected error
$s->query_safe("SELECT injection_points_detach('flux-delete-after-undo-prepare')");
$s->query_safe('DELETE FROM f WHERE id = 5');
pass('a fresh DML on the same backend succeeded after a top-level abort');

$s->quit;

# id 2 updated; ids 3 and 5 deleted; ids 1 and 4 survived their aborted deletes.
is( $node->safe_psql('postgres', 'SELECT count(*) FROM f'),
	'18', 'exactly the committed DML took effect');
is( $node->safe_psql('postgres', "SELECT v FROM f WHERE id = 2"),
	'x2u', 'the post-error UPDATE committed');
is( $node->safe_psql('postgres',
		"SELECT string_agg(id::text, ',' ORDER BY id) FROM f WHERE id <= 5"),
	'1,2,4', 'the rolled-back/aborted deletes left their rows; committed ones gone');

$node->stop;
done_testing();
