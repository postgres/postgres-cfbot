# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Check the compact header and tuple layout through rollback and replay.
# The next UPDATE must still use byte-range CAS WAL.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"autovacuum = off\nwal_consistency_checking = all\n");
$node->start;
if (   !$node->check_extension('pageinspect')
	|| !$node->check_extension('pg_walinspect'))
{
	plan skip_all => 'pageinspect or pg_walinspect not installed';
}

# The undo apply worker connects to the database it reverts.  The worker
# case uses its own database, rr, so that the worker can be kept out of it
# (ALLOW_CONNECTIONS false) while its rollback is checked to be pending.
$node->safe_psql('postgres', 'CREATE DATABASE rr');
foreach my $db ('postgres', 'rr')
{
	$node->safe_psql($db,
		'CREATE EXTENSION pageinspect; CREATE EXTENSION pg_walinspect;');
}
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

# The first change to the page after VACUUM marked it all-visible clears
# the visibility map in its own record, which the CAS record cannot do, so it
# takes the exclusive path.  Make it to the other row, so that the UPDATEs
# below find a page that is not all-visible.
sub make_table
{
	my ($db, $t) = @_;
	$node->safe_psql(
		$db, qq{
CREATE TABLE $t (id int, v int) USING flux;
INSERT INTO $t VALUES (1, 0), (2, 0);
VACUUM $t;
UPDATE $t SET v = 0 WHERE id = 2;
});
}

# Two int columns: 36-byte header, bitmap and alignment to 40, then 8 bytes.
sub layout
{
	my ($n, $db, $t, $id) = @_;
	return $n->safe_psql(
		$db, qq{
SELECT lp_len || ':' || get_byte(get_raw_page('$t', 0), lp_off + 36)
  FROM heap_page_items(get_raw_page('$t', 0))
 WHERE lp = (SELECT (ctid::text::point)[1]::int FROM $t WHERE id = $id)});
}

# The FLUX WAL records that $sql emits.
sub flux_records
{
	my ($db, $sql) = @_;
	my $s = $node->safe_psql($db, 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql($db, $sql);
	my $e = $node->safe_psql($db, 'SELECT pg_current_wal_insert_lsn()');
	return $node->safe_psql(
		$db, qq{
SELECT string_agg(record_type, ',') FROM pg_get_wal_records_info('$s', '$e')
 WHERE resource_manager = 'FLUX'});
}

my $rows = sub {
	my $t = shift;
	return "SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM $t";
};

# What is left on the page once a rollback is done.
sub check_row
{
	my ($db, $t, $id, $want, $what) = @_;
	is($node->safe_psql($db, $rows->($t)), $want, "$what: rows restored");
	is(layout($node, $db, $t, $id),
		'48:255', "$what: header and bitmap layout matches on the primary");
	$node->wait_for_catchup($standby);
	is(layout($standby, $db, $t, $id),
		'48:255', "$what: header and bitmap layout matches on the standby");
	is(flux_records($db, "UPDATE $t SET v = v + 1 WHERE id = $id"),
		'CAS_UPDATE_UNDO', "$what: the next UPDATE takes the CAS path");
	is(layout($node, $db, $t, $id),
		'48:255',
		"$what: header and bitmap layout matches after that UPDATE");
}

# Inline ROLLBACK.  The control shows that an UPDATE of the row takes the
# CAS path in this setup in the first place.
make_table('postgres', 'ti');
is(flux_records('postgres', 'UPDATE ti SET v = v + 1 WHERE id = 1'),
	'CAS_UPDATE_UNDO',
	'control: an UPDATE of a fresh row takes the CAS path');
$node->safe_psql('postgres',
	'BEGIN; UPDATE ti SET v = v + 10 WHERE id = 1; ROLLBACK;');
check_row('postgres', 'ti', 1, '1:1,2:0', 'inline ROLLBACK');

# Two in a row: on the old code the second UPDATE took the slow path over
# the stale word, and its rollback restored the stale word again.
$node->safe_psql('postgres',
	'BEGIN; UPDATE ti SET v = v + 10 WHERE id = 1; ROLLBACK;');
$node->safe_psql('postgres',
	'BEGIN; UPDATE ti SET v = v + 10 WHERE id = 1; ROLLBACK;');
check_row('postgres', 'ti', 1, '1:2,2:0', 'two inline ROLLBACKs');

# ROLLBACK TO SAVEPOINT.
make_table('postgres', 'ts');
$node->safe_psql('postgres',
	'BEGIN; SAVEPOINT s; UPDATE ts SET v = v + 10 WHERE id = 1; ROLLBACK TO s; COMMIT;'
);
check_row('postgres', 'ts', 1, '1:0,2:0', 'ROLLBACK TO SAVEPOINT');

# An ERROR inside what used to be the locked window: the write-skew UPDATE
# fails in CheckForSerializableConflictIn, after the eligibility checks and
# before the page is written.
make_table('postgres', 'tx');
my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $s2 = $node->background_psql('postgres', on_error_stop => 0);
$s1->query_safe(
	'BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT v FROM tx WHERE id = 1;');
$s2->query_safe(
	'BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT v FROM tx WHERE id = 2;');
$s1->query_safe('UPDATE tx SET v = 1 WHERE id = 2;');
$s1->query_safe('COMMIT;');
$s2->{stderr} = '';
$s2->query('UPDATE tx SET v = 1 WHERE id = 1;');
like(
	$s2->{stderr},
	qr/could not serialize access.*during write/s,
	'SSI: the write-skew UPDATE fails while writing');
$s2->{stderr} = '';
$s2->query_safe('ROLLBACK;');
$s1->quit;
$s2->quit;
check_row('postgres', 'tx', 1, '1:0,2:1', 'SSI serialization failure');

# Crash recovery.  A committed transaction flushes the WAL, the open
# UPDATE's record with it, so the restarted server has it to undo.  Its
# value, 16908801 = 0x01020201, reads the same in either byte order, so the
# page can be searched for it.
my $aborted = sub {
	my $t = shift;
	return
	  "SELECT position('\\x01020201'::bytea IN get_raw_page('$t', 0)) > 0";
};
make_table('postgres', 'tc');
my $s3 = $node->background_psql('postgres', on_error_stop => 0);
$s3->query_safe('BEGIN; UPDATE tc SET v = 16908801 WHERE id = 1;');
$node->safe_psql('postgres', 'CREATE TABLE flush_wal ()');
is($node->safe_psql('postgres', $aborted->('tc')),
	't', 'crash: the open UPDATE is on the page');
$node->stop('immediate');
eval { $s3->quit; };
$node->start;
ok($node->poll_query_until('postgres', $aborted->('tc'), 'f'),
	'crash: the UPDATE was undone');
check_row('postgres', 'tc', 1, '1:0,2:0', 'crash recovery');

# The undo apply worker.
$node->safe_psql('postgres',
	'ALTER SYSTEM SET undo_rollback_overflow_size = 0');
$node->reload;
ok($node->poll_query_until('rr', 'SHOW undo_rollback_overflow_size', '0'),
	'worker: undo_rollback_overflow_size is 0');
make_table('rr', 'tw');
my $bg = $node->background_psql('rr', on_error_stop => 0);
$node->safe_psql('postgres', 'ALTER DATABASE rr ALLOW_CONNECTIONS false');
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
		  WHERE backend_type = 'undo apply worker'}),
	'worker: no undo apply worker is connected');
$bg->query_safe('BEGIN; UPDATE tw SET v = 16908801 WHERE id = 1; ROLLBACK;');
is($bg->query_safe($aborted->('tw')),
	't', 'worker: the rollback is left to the worker');
$bg->quit;
$node->safe_psql('postgres', 'ALTER DATABASE rr ALLOW_CONNECTIONS true');
ok($node->poll_query_until('rr', $aborted->('tw'), 'f'),
	'worker: the worker applied the rollback');
check_row('rr', 'tw', 1, '1:0,2:0', 'undo apply worker');

$node->wait_for_catchup($standby);
foreach my $c (
	[ 'postgres', 'ti' ],
	[ 'postgres', 'ts' ],
	[ 'postgres', 'tx' ],
	[ 'postgres', 'tc' ],
	[ 'rr', 'tw' ])
{
	is( $standby->safe_psql($c->[0], $rows->($c->[1])),
		$node->safe_psql($c->[0], $rows->($c->[1])),
		"$c->[1]: the standby sees the primary's rows");
}
ok($standby->is_alive, 'standby is alive');
ok(!$standby->log_contains('inconsistent page found'),
	'standby found no inconsistent page');

$standby->stop('fast', fail_ok => 1);
$node->stop;
done_testing();
