# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that catalog commits in other databases do not disturb logical
# decoding: the snapshot rebuild and the distribution of the snapshot and
# the invalidation messages to the in-progress transactions are skipped for
# them unless shared catalogs were touched, and the decoding of the slot's
# own database produces correct results either way.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(allows_streaming => 'logical');
$node->start;

# db1 holds the replication slot; db2 acts as the other database whose
# catalog commits must not disturb the decoding of db1.
$node->safe_psql('postgres', 'CREATE DATABASE db1');
$node->safe_psql('postgres', 'CREATE DATABASE db2');

$node->safe_psql('db1', 'CREATE TABLE t1(a int)');
$node->safe_psql('db1',
	"SELECT pg_create_logical_replication_slot('slot_db1', 'test_decoding')");
$node->safe_psql('db2', 'CREATE TABLE t2(a int)');
$node->safe_psql('db2', 'CREATE ROLE regress_role1');
$node->safe_psql('db2', 'CREATE PUBLICATION pub0 FOR ALL TABLES');

my $get_changes = qq(
	SET log_min_messages = 'debug1';
	SELECT data FROM pg_logical_slot_get_changes('slot_db1', NULL, NULL,
		'include-xids', '0', 'skip-empty-xacts', '1'));

# Shared-catalog DDL committed in db2 while a transaction is in progress in
# db1.  The commit touches a shared catalog, so the snapshot and the
# invalidations are still distributed; the decoding of db1's transaction
# must produce correct results.
my $session1 = $node->background_psql('db1', on_error_stop => 1);
$session1->query_safe('BEGIN');
$session1->query_safe('INSERT INTO t1 VALUES (1)');

my $session_role = $node->background_psql('db2', on_error_stop => 1);
$session_role->query_safe('BEGIN');
$session_role->query_safe('ALTER ROLE regress_role1 CONNECTION LIMIT 10');
my $xid_role = $session_role->query_safe('SELECT pg_current_xact_id()');
chomp($xid_role);
$session_role->query_safe('COMMIT');
$session_role->quit;

$session1->query_safe('COMMIT');
$session1->quit;

my $result = $node->safe_psql('db1', $get_changes);
is($result, qq(BEGIN
table public.t1: INSERT: a[integer]:1
COMMIT),
	'shared-catalog DDL in db2 does not disturb decoding of db1');

my $log = slurp_file($node->logfile);
like($log,
	qr/transaction $xid_role committed in database \d+ has catalog changes relevant/,
	'shared-catalog DDL in db2 classified as relevant');

# A catalog DDL committed in db2, with a shared-catalog DDL rolled back to
# a savepoint, while a transaction is in progress in db1.  The invalidation
# messages of the rolled back savepoint were WAL-logged before the rollback
# and cannot be retracted from the reorder buffer, but the commit record
# carries only the surviving messages, which touch no shared catalog, so
# the snapshot distribution is skipped; the decoding of db1's transaction
# must still produce correct results.
$session1 = $node->background_psql('db1', on_error_stop => 1);
$session1->query_safe('BEGIN');
$session1->query_safe('INSERT INTO t1 VALUES (2)');

my $session2 = $node->background_psql('db2', on_error_stop => 1);
$session2->query_safe('BEGIN');
$session2->query_safe('ALTER TABLE t2 RENAME TO t2_new');
$session2->query_safe('SAVEPOINT s1');
$session2->query_safe('ALTER ROLE regress_role1 RENAME TO regress_role2');
$session2->query_safe('ROLLBACK TO SAVEPOINT s1');
my $xid_savepoint = $session2->query_safe('SELECT pg_current_xact_id()');
chomp($xid_savepoint);
$session2->query_safe('COMMIT');
$session2->quit;

$session1->query_safe('COMMIT');
$session1->quit;

$result = $node->safe_psql('db1', $get_changes);
is($result, qq(BEGIN
table public.t1: INSERT: a[integer]:2
COMMIT),
	'rolled back shared-catalog DDL in db2 does not disturb decoding of db1');

$log = slurp_file($node->logfile);
like($log,
	qr/transaction $xid_savepoint committed in database \d+ does not have catalog changes relevant/,
	'ddl+savepoint+ddl+rollback in db2 classified as irrelevant');

# A publication parameter change on a FOR ALL TABLES publication in db2
# while a transaction is in progress in db1.  The commit carries a
# whole-relcache invalidation broadcast (dbId and relId both 0), which is
# conservatively treated as touching a shared catalog, so the snapshot and
# the invalidations are still distributed; the decoding of db1's
# transaction must produce correct results.
$session1 = $node->background_psql('db1', on_error_stop => 1);
$session1->query_safe('BEGIN');
$session1->query_safe('INSERT INTO t1 VALUES (3)');

my $session_pub = $node->background_psql('db2', on_error_stop => 1);
$session_pub->query_safe('BEGIN');
$session_pub->query_safe(
	"ALTER PUBLICATION pub0 SET (publish = 'insert, update, delete')");
my $xid_pub = $session_pub->query_safe('SELECT pg_current_xact_id()');
chomp($xid_pub);
$session_pub->query_safe('COMMIT');
$session_pub->quit;

$session1->query_safe('COMMIT');
$session1->quit;

$result = $node->safe_psql('db1', $get_changes);
is($result, qq(BEGIN
table public.t1: INSERT: a[integer]:3
COMMIT),
	'whole-relcache broadcast in db2 does not disturb decoding of db1');

$log = slurp_file($node->logfile);
like($log,
	qr/transaction $xid_pub committed in database \d+ has catalog changes relevant/,
	'whole-relcache broadcast in db2 classified as relevant');


# Many catalog commits in db2 with the xmin of the historic snapshots pinned
# by an in-progress transaction.  Each such commit is irrelevant to this slot,
# so no snapshot is distributed and the reorder buffer must not spill.
my $session3 = $node->background_psql('db2', on_error_stop => 1);
$session3->query_safe('BEGIN');
$session3->query_safe('INSERT INTO t2_new VALUES (1)');

my $session4 = $node->background_psql('db2', on_error_stop => 1);
for my $i (1 .. 300)
{
	$session4->query_safe("CREATE TABLE spill_t_$i(a int)");
}
$session4->quit;

$session3->query_safe('COMMIT');
$session3->quit;

$node->safe_psql('db1', 'CHECKPOINT');
$node->safe_psql(
	'db1', qq(
	SET logical_decoding_work_mem = '64kB';
	SELECT count(*) FROM pg_logical_slot_get_changes('slot_db1', NULL, NULL,
		'include-xids', '0', 'skip-empty-xacts', '1')));

$result = $node->safe_psql('db1',
	"SELECT spill_bytes FROM pg_stat_replication_slots WHERE slot_name = 'slot_db1'");
is($result, '0',
	'catalog commits in db2 do not spill the reorder buffer');

done_testing();
