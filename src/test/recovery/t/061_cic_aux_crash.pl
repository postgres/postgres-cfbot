# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that a leftover auxiliary (STIR) index of a CREATE INDEX
# CONCURRENTLY interrupted by a crash behaves gracefully afterwards.
# The auxiliary index is unlogged, so after a crash its main fork is
# reset to the init fork; that init fork must contain a valid metapage
# (with inserts disabled), so that inserts into the table are silently
# skipped and VACUUM merely warns instead of failing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql(
	'postgres', q[
	CREATE TABLE tab (i int);
	INSERT INTO tab SELECT generate_series(1, 1000);
]);

# Session 1: hold an open write transaction, so that CIC parks in its
# first WaitForLockers, before the auxiliary index is built.
my $blocker1 = $node->background_psql('postgres', on_error_stop => 0);
$blocker1->query_safe(
	q[
BEGIN;
INSERT INTO tab VALUES (0);
]);

# Session 2: start CREATE INDEX CONCURRENTLY; it creates the auxiliary
# index and then blocks waiting for session 1.
my $cic = $node->background_psql('postgres', on_error_stop => 0);
$cic->query_until(
	qr/starting_cic/, q[
\echo starting_cic
CREATE INDEX CONCURRENTLY idx ON tab (i);
]);

# Session 3: another open write transaction.  It will keep CIC parked in
# a later WaitForLockers, after the auxiliary index has been built and
# marked ready for inserts.
my $blocker2 = $node->background_psql('postgres', on_error_stop => 0);
$blocker2->query_safe(
	q[
BEGIN;
INSERT INTO tab VALUES (0);
]);

# Let CIC proceed to build the auxiliary index and block on session 3.
$blocker1->query_safe(q[COMMIT;]);
$blocker1->quit;

# Wait until the auxiliary index exists and accepts inserts.
$node->poll_query_until(
	'postgres', q[
SELECT indisready FROM pg_index
 WHERE indexrelid = (SELECT oid FROM pg_class WHERE relname = 'idx_ccaux')])
  or die "timed out waiting for auxiliary index to become ready";

# Crash the server.  The CIC never completes, leaving behind the invalid
# target index and its insert-ready auxiliary index; the auxiliary index
# is reset to its init fork during recovery.
$node->stop('immediate');
eval { $cic->quit };
eval { $blocker2->quit };
$node->start;

# Remember the size of the auxiliary index right after recovery: it has
# been reset to a copy of its init fork.
my $aux_size = $node->safe_psql('postgres',
	q[SELECT pg_relation_size('idx_ccaux')]);

# Inserts into the table must be silently skipped by the leftover
# auxiliary index, not fail.  Insert clearly more than a page worth of
# TIDs, so that any accidental acceptance of them is visible in the
# index size.
$node->safe_psql('postgres',
	q[INSERT INTO tab SELECT generate_series(1001, 11000)]);
is( $node->safe_psql('postgres',
		q[SELECT count(*) FROM tab WHERE i BETWEEN 1001 AND 11000]),
	'10000', 'inserts into table with leftover auxiliary index work');

# The reset metapage must have inserts disabled: the index may not grow.
# If the init fork had not been properly WAL-logged, the reset leaves
# behind a zeroed metapage, which reads as "inserts allowed", and the
# insert above would have extended the index.
is( $node->safe_psql('postgres', q[SELECT pg_relation_size('idx_ccaux')]),
	$aux_size, 'leftover auxiliary index ignores inserts');

# VACUUM must only warn about the leftover, not fail.
my ($ret, $stdout, $stderr) = $node->psql('postgres', q[VACUUM tab;]);
is($ret, 0, 'VACUUM of table with leftover auxiliary index succeeds');
like(
	$stderr,
	qr/needs to be dropped/,
	'VACUUM warns about leftover auxiliary index');

# Dropping the invalid target index must get rid of the auxiliary index
# as well.
$node->safe_psql('postgres', q[DROP INDEX idx;]);
is( $node->safe_psql(
		'postgres',
		q[SELECT count(*) FROM pg_class WHERE relname IN ('idx', 'idx_ccaux')]),
	'0',
	'leftover indexes are gone after DROP INDEX');

done_testing();
