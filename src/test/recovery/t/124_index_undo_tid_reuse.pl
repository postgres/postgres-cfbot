
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Index UNDO applied after the abort is visible must not kill a committed
# entry that reuses the aborted heap TID.
#
# nbtree and hash index UNDO find "their" entry by (heap TID, length, key
# digest).  That identifies it only while the aborted heap tuple still holds
# the TID.  When the apply is deferred -- ATM instant abort handed to the
# logical revert worker, or crash recovery deferring the chain to the worker
# -- VACUUM can remove the aborted heap tuples and their index entries in
# between, and a committed INSERT can reuse the TIDs with the same keys.  The
# deferred apply must then leave those committed entries alone: index scans
# must still find every row, and the primary key must still reject a
# duplicate.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $nrows = 200;

my $node = PostgreSQL::Test::Cluster->new('index_undo_tid_reuse');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
logical_revert_naptime = 100
# Put the UNDO into the WAL as it is written, for the crash case.
undo_batch_record_limit = 100
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('amcheck')
	|| !$node->check_extension('test_fileops'))
{
	plan skip_all =>
	  'injection_points, amcheck or test_fileops not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION amcheck;
CREATE EXTENSION test_fileops;
-- Number of rows with key 1..n found by index scans on column col.
CREATE FUNCTION idx_found(tab regclass, col text, n int) RETURNS int
LANGUAGE plpgsql AS $$
DECLARE
  c int := 0;
  f int;
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  FOR i IN 1..n LOOP
    EXECUTE format('SELECT count(*) FROM %s WHERE %I = $1', tab, col)
      INTO f USING i;
    c := c + f;
  END LOOP;
  RETURN c;
END $$;
));

sub create_table
{
	my ($t) = @_;
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE $t (id int PRIMARY KEY, v int) USING heap
  WITH (index_undo = on, autovacuum_enabled = off);
CREATE INDEX ${t}_v_hash ON $t USING hash (v);
));
}

sub insert_sql
{
	my ($t) = @_;
	return "INSERT INTO $t SELECT g, g FROM generate_series(1, $nrows) g "
	  . "RETURNING ctid;";
}

# VACUUM away the aborted rows, then commit the same keys again, which
# reuses the freed line pointers.  Returns how many TIDs were reused.
sub vacuum_and_reinsert
{
	my ($t, $aborted_ctids) = @_;
	my %aborted = map { $_ => 1 } split /\n/, $aborted_ctids;

	$node->safe_psql('postgres', "VACUUM (INDEX_CLEANUP on) $t");
	is($node->safe_psql('postgres', "SELECT count(*) FROM $t"),
		'0', "$t: aborted rows gone after VACUUM");
	my $new = $node->safe_psql('postgres', insert_sql($t));
	return scalar grep { $aborted{$_} } split /\n/, $new;
}

sub wait_atm_drained
{
	$node->poll_query_until('postgres',
		'SELECT test_fileops_atm_oldest_lsn() IS NULL')
	  or die 'timed out waiting for the ATM to drain';
}

sub check_table
{
	my ($t, $label) = @_;

	is($node->safe_psql('postgres', "SELECT count(*) FROM $t"),
		$nrows, "$label: seqscan sees every row");
	like(
		$node->safe_psql(
			'postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off;
			 EXPLAIN (COSTS OFF) SELECT * FROM $t WHERE v = 1"),
		qr/Index Scan using ${t}_v_hash/,
		"$label: hash lookups use the hash index");
	is( $node->safe_psql('postgres', "SELECT idx_found('$t', 'id', $nrows)"),
		$nrows,
		"$label: btree index scans find every row");
	is($node->safe_psql('postgres', "SELECT idx_found('$t', 'v', $nrows)"),
		$nrows, "$label: hash index scans find every row");

	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', "INSERT INTO $t VALUES (1, 1)");
	like(
		$stderr,
		qr/duplicate key value violates unique constraint/,
		"$label: primary key rejects a duplicate of a committed key");

	($ret, $stdout, $stderr) = $node->psql('postgres',
		"SELECT bt_index_check('${t}_pkey', true, true)");
	is($ret, 0, "$label: bt_index_check passes")
	  or diag($stderr);
}

###########################################################################
# 1. ATM instant abort: the logical revert worker applies the chain later.
###########################################################################
create_table('t_atm');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('logical-revert-before-process', 'wait')"
);

my $aborted = $node->safe_psql(
	'postgres', qq(
SET undo_instant_abort_threshold = 0;
BEGIN;
@{[insert_sql('t_atm')]}
ROLLBACK;
));
$aborted =~ s/^(SET|BEGIN|ROLLBACK)\n?//mg;
isnt(test_fileops_atm_pending(), 0, 'ATM: rollback went to the ATM');

$node->wait_for_event('logical revert worker',
	'logical-revert-before-process');

my $reused = vacuum_and_reinsert('t_atm', $aborted);
cmp_ok($reused, '>', 0, "ATM: committed rows reuse $reused aborted TIDs");

$node->safe_psql(
	'postgres', q(
SELECT injection_points_detach('logical-revert-before-process');
SELECT injection_points_wakeup('logical-revert-before-process');
));
wait_atm_drained();
check_table('t_atm', 'ATM');

sub test_fileops_atm_pending
{
	return $node->safe_psql('postgres',
		'SELECT (test_fileops_atm_oldest_lsn() IS NOT NULL)::int');
}

###########################################################################
# 2. Crash: recovery defers the chain to the worker, which runs only after
# VACUUM and the committed re-insert.
###########################################################################
create_table('t_crash');
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');

my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
my $crash_ctids = $bg->query_safe(insert_sql('t_crash'));
# Make the inserts and their UNDO durable before the crash.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
eval { $bg->quit; };
$node->start;

is(test_fileops_atm_pending(),
	1, 'crash: recovery deferred the chain to the ATM');
$reused = vacuum_and_reinsert('t_crash', $crash_ctids);
cmp_ok($reused, '>', 0, "crash: committed rows reuse $reused aborted TIDs");

$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
wait_atm_drained();
check_table('t_crash', 'crash');

$node->stop;
done_testing();
