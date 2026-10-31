# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# An UNDO record that fails to apply must not be counted as applied.
#
# Inline rollback (top-level abort and ROLLBACK TO SAVEPOINT) that cannot apply
# its whole chain hands it to the logical revert worker through the ATM.  For a
# subtransaction the hand-off covers only the subtransaction's part of the
# chain: the parent goes on to commit, and its work must not be reverted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_apply_failure');
$node->init;
# No revert worker at first, so the hand-offs stay observable in the ATM.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key) WITH (index_undo = on);
CREATE TABLE p (id int primary key) WITH (index_undo = on);
CREATE TABLE c (id int primary key) WITH (index_undo = on);
});

# Top-level abort whose chain holds one record that cannot be applied.
my $xid = $node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t SELECT g FROM generate_series(1, 10) g;
SELECT idxundo_write_failing_undo();
SELECT pg_current_xact_id();
ROLLBACK;
});
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$xid' AND stop_lsn IS NULL"
	),
	'1',
	'abort whose inline apply failed is handed to the ATM');
is($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('t_pkey')"),
	'10', 'the records that could be applied were applied inline');

# ROLLBACK TO SAVEPOINT whose chain holds such a record; the parent commits.
$node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO p SELECT g FROM generate_series(1, 10) g;
SAVEPOINT s;
INSERT INTO c SELECT g FROM generate_series(1, 10) g;
SELECT idxundo_write_failing_undo();
ROLLBACK TO SAVEPOINT s;
COMMIT;
});
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE stop_lsn IS NOT NULL"
	),
	'1',
	'subtransaction whose inline apply failed hands its bounded chain to the ATM'
);
is($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('c_pkey')"),
	'10', 'the subtransaction records that could be applied were applied');
is($node->safe_psql('postgres', "SELECT idxundo_count_dead_all('p_pkey')"),
	'0', 'the parent\'s index entries are untouched');

# A savepoint whose failing chain is a temporary one.  It cannot be handed
# off: a revert worker cannot reach this backend's temporary relations.  (Nor
# can recovery, which never applies TEMP or UNLOGGED UNDO.)  A failed chain at
# two levels would also have collided on one ATM key.
$node->safe_psql(
	'postgres', q{
BEGIN;
SAVEPOINT s;
SELECT idxundo_write_failing_undo('t');
ROLLBACK TO SAVEPOINT s;
COMMIT;
});
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM idxundo_atm_entries()"),
	'2',
	'a failed temporary chain is not handed to the ATM');
like(
	slurp_file($node->logfile),
	qr/could not apply the temporary UNDO of rolled-back subtransaction/,
	'the failed temporary chain is reported');

# An inline apply that throws (rather than reporting a failed record) falls
# back to the ATM too, without leaving the abort's interrupt holdoff unbalanced.
SKIP:
{
	skip 'Injection points not supported by this build', 2
	  unless $ENV{enable_injection_points} eq 'yes';

	$node->safe_psql(
		'postgres', q{
CREATE EXTENSION injection_points;
SELECT injection_points_set_local();
SELECT injection_points_attach('undo-apply-after-batch',
  'test_idxundo', 'idxundo_error_with_resources', ''::bytea);
BEGIN;
INSERT INTO t SELECT g FROM generate_series(11, 20) g;
ROLLBACK;
BEGIN;
INSERT INTO p SELECT g FROM generate_series(11, 20) g;
SAVEPOINT s;
INSERT INTO c SELECT g FROM generate_series(11, 20) g;
ROLLBACK TO SAVEPOINT s;
COMMIT;
});
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM idxundo_atm_entries()"),
		'4',
		'aborts whose inline apply threw are handed to the ATM');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM p'),
		'20', 'the parent of the subtransaction committed');
}

# Now let the revert worker run.  The ATM survives the restart; the worker
# retries both chains (the failing record fails again, in a process that
# does not know its resource manager) and forgets them.  Walking the
# subtransaction's entry must stop at the parent's part of the chain.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'logical revert worker processed both hand-offs');
is( $node->safe_psql('postgres', "SELECT idxundo_count_dead_all('p_pkey')"),
	'0',
	'reverting the subtransaction did not revert its committed parent');
is( $node->safe_psql(
		'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM p WHERE id BETWEEN 1 AND 20;
}),
	$node->safe_psql('postgres', 'SELECT count(*) FROM p'),
	'the committed parent\'s rows are reachable through the index');

$node->stop;
done_testing();
