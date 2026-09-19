# Copyright (c) 2026, PostgreSQL Global Development Group

# UndoRecordBulkFetch() walks a transaction's UNDO chain backward, and for each
# record looks up its undo log with UndoLogGet() to read the log's persistence.
# UndoLogGet() returns NULL for a stale UndoRecPtr whose log number decodes to 0
# (a slot a concurrent transaction reset while only a shared content lock was
# held -- "seen from GetLockerTransInfo -> UndoFetchRecord under a 16-way
# concurrent load", get_undo_log_by_number()).  Every other UndoLogGet() caller
# treats that NULL as "the record is gone" and stops; this one dereferenced it
# (log->meta.persistence) and crashed.
#
# idxundo_bulk_fetch_stale_log() calls UndoRecordBulkFetch() with exactly such
# a from_urecptr (log number 0, non-zero offset).  Before the fix the backend
# crashes; with the fix it returns 0 (the chain's log is gone).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_bulk_fetch_stale_log');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

# A crash would drop the connection; psql returns non-zero and the server logs
# a crash.  The fix returns 0 and the connection survives.
my ($ret, $out, $err) =
  $node->psql('postgres', 'SELECT idxundo_bulk_fetch_stale_log()');
is("$ret:$out", '0:0',
	'UndoRecordBulkFetch on a stale (log 0) UndoRecPtr returns 0, no crash');

# A clean query afterwards proves the backend did not crash and restart.
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'server still up');

unlike(slurp_file($node->logfile),
	qr/server process .* was terminated|TRAP:|segmentation fault/,
	'no crash in the server log');

$node->stop;
done_testing();
