# Copyright (c) 2026, PostgreSQL Global Development Group
#
# ROLLBACK TO SAVEPOINT of a subtransaction that gave a relation new storage
# (CREATE TABLE, TRUNCATE) and then updated rows in it.  The subtransaction's
# abort record drops that storage on redo, so every UNDO apply record for it
# must come before the abort record.  FLUX used to write them after it: a
# streaming standby then replayed an UNDO apply record for a block of a file
# it had just unlinked and stopped with "WAL contains references to invalid
# pages", and so did crash recovery of the primary.  Heap is the control,
# except for REINDEX in the subtransaction, which hit heap's index UNDO too.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_consistency_checking = all
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE ${am}_keep (id int PRIMARY KEY, v int) USING $am;
INSERT INTO ${am}_keep SELECT g, 0 FROM generate_series(1, 10) g;
));

	# Tables created in the subtransaction, written, updated, rolled back;
	# the parent commits.  This is the join_hash regression test's shape.
	$node->safe_psql(
		'postgres', qq(
BEGIN;
INSERT INTO ${am}_keep VALUES (0, 0);
SAVEPOINT s;
CREATE TABLE ${am}_a (id int) USING $am;
CREATE TABLE ${am}_b (id int) USING $am;
INSERT INTO ${am}_a VALUES (1);
INSERT INTO ${am}_b VALUES (2);
UPDATE ${am}_b SET id = 2;
ROLLBACK TO s;
COMMIT;
));

	# The same, with an index and a whole-transaction ROLLBACK after it.
	$node->safe_psql(
		'postgres', qq(
BEGIN;
SAVEPOINT s;
CREATE TABLE ${am}_c (id int PRIMARY KEY, v int) USING $am;
INSERT INTO ${am}_c SELECT g, 0 FROM generate_series(1, 50) g;
UPDATE ${am}_c SET v = v + 1;
ROLLBACK TO s;
ROLLBACK;
));

	# An existing table TRUNCATEd in the subtransaction: the new file is
	# dropped by the subtransaction's abort, the old one comes back.
	$node->safe_psql(
		'postgres', qq(
BEGIN;
INSERT INTO ${am}_keep VALUES (-1, 0);
SAVEPOINT s;
TRUNCATE ${am}_keep;
INSERT INTO ${am}_keep SELECT g, 1 FROM generate_series(1, 6) g;
UPDATE ${am}_keep SET v = 2;
ROLLBACK TO s;
COMMIT;
));

	# Rows written in the subtransaction to the old index files, then a
	# REINDEX: the index UNDO names the index by OID, which now maps to the
	# REINDEXed file the abort drops.  heap's index UNDO (the cluster-wide
	# engine) and FLUX's in-place index reversal both have to leave it alone.
	$node->safe_psql(
		'postgres', qq(
CREATE INDEX ${am}_keep_v ON ${am}_keep (v);
BEGIN;
INSERT INTO ${am}_keep VALUES (-2, 0);
SAVEPOINT s;
INSERT INTO ${am}_keep SELECT g, g FROM generate_series(100, 110) g;
UPDATE ${am}_keep SET v = v + 1000 WHERE id BETWEEN 1 AND 5;
REINDEX TABLE ${am}_keep;
ROLLBACK TO s;
COMMIT;
));

	foreach my $scope ('top', 'sub')
	{
		my $savepoint = $scope eq 'sub' ? 'SAVEPOINT s;' : '';
		my $rollback =
		  $scope eq 'sub' ? 'ROLLBACK TO s; COMMIT;' : 'ROLLBACK;';
		$node->safe_psql(
			'postgres', qq(
BEGIN;
$savepoint
UPDATE ${am}_keep SET v = v + 1000 WHERE id BETWEEN 1 AND 5;
REINDEX TABLE ${am}_keep;
$rollback
));
		is( $node->safe_psql(
				'postgres', "SELECT bt_index_check('${am}_keep_v', true)"),
			'',
			"$am: $scope rollback restores the old index file");
	}

	is( $node->safe_psql(
			'postgres', "SELECT count(*), sum(v) FROM ${am}_keep"),
		'13|0',
		"$am: the parent's rows survive the savepoint rollbacks");
	is( $node->safe_psql(
			'postgres', qq(
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM ${am}_keep WHERE v >= 0;
)),
		'13',
		"$am: the index has exactly the parent's rows");
}

$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'standby replayed the savepoint rollbacks');
foreach my $am ('heap', 'flux')
{
	is( $standby->safe_psql(
			'postgres', "SELECT count(*), sum(v) FROM ${am}_keep"),
		'13|0',
		"$am: standby has the parent's rows");
}
unlike(
	slurp_file($standby->logfile),
	qr/invalid pages|does not exist.*CONTEXT:  WAL redo at .* Undo|PANIC|inconsistent page found/s,
	'standby log has no invalid-page reference or PANIC');

# Crash recovery of the primary replays the same WAL from the backup point.
$node->stop('immediate');
$node->start;
foreach my $am ('heap', 'flux')
{
	is( $node->safe_psql(
			'postgres', "SELECT count(*), sum(v) FROM ${am}_keep"),
		'13|0',
		"$am: rows after crash recovery");
}
unlike(
	slurp_file($node->logfile),
	qr/invalid pages|PANIC/,
	'primary crash recovery found no invalid page');

$standby->stop;
$node->stop;
done_testing();
