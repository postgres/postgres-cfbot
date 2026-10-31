
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Guard for a crash-recovery bug in the per-backend UNDO log: the startup
# process died with "xid to undo log number map not initialized" when the first
# undo record of a transaction replayed after a checkpoint had no preceding
# XLOG_UNDOLOG_ATTACH/META for its xid.
#
# Root cause (fixed): LogUndoMetaData() decided whether to emit the xid->logno
# mapping record by comparing the undo log's last-logged LSN against the
# backend-cached redo point from GetFullPageWriteInfo().  That cached copy is
# refreshed only inside XLogInsertRecord, so between a backend's WAL inserts it
# can lag a concurrent checkpoint; the mapping record was then skipped and the
# undo record that followed became the first after the new redo point with
# nothing mapping its xid.  The fix compares against the current redo point
# (GetRedoRecPtr()); FLUX redo also re-seeds the map from the undo record
# pointer the forward record already carries, so recovery no longer depends on
# the mapping record's ordering at all.
#
# This is a GUARD, not a reproduction: it passes on the base too, because the
# bug needs a checkpoint to advance the redo point in the narrow window between
# a backend's own WAL inserts, which this cannot force deterministically.  The
# reliable reproduction is recovery/137_flux_vacuum_parallel under
# wal_consistency_checking (fails ~1/3 of runs on the base, 0/15 after the fix).
# Here we exercise the shape -- a transaction whose undo spans a checkpoint,
# then an immediate crash -- for both access methods, with the heap run as a
# control (heap writes no undo and so cannot hit this).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('undo_attach_order');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

sub run_am
{
	my ($am) = @_;

	$node->safe_psql(
		'postgres', qq{
DROP TABLE IF EXISTS u_$am;
CREATE TABLE u_$am (id int, v int) USING $am WITH (autovacuum_enabled = off);
INSERT INTO u_$am SELECT g, g FROM generate_series(1, 20000) g;
CHECKPOINT;
});

	my $h = $node->background_psql('postgres', on_error_stop => 0);
	$h->query_safe("BEGIN");
	# First undo op of the transaction: emits ATTACH + META, grows the log.
	$h->query_safe("DELETE FROM u_$am WHERE id <= 10000");

	# Advance the redo point from another backend while the held session is
	# idle, then do a second undo op in the SAME transaction (no new ATTACH):
	# only a META could carry the mapping for its first record after the
	# checkpoint.
	$node->safe_psql('postgres', "CHECKPOINT");
	$h->query_safe("DELETE FROM u_$am WHERE id = 20000");

	# Crash before commit; recovery must replay the second op and roll back.
	$node->stop('immediate');
	eval { $h->quit };

	$node->start;

	is( $node->safe_psql('postgres', "SELECT count(*) FROM u_$am"),
		20000,
		"$am: recovery survives an undo op after a checkpoint-advanced redo point"
	);
}

run_am('heap');
run_am('flux');

$node->stop;
done_testing();
