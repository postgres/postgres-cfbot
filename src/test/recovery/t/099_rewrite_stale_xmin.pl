# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Test that heap rewrites (CLUSTER, VACUUM FULL, REPACK) and index builds
# work in the case when the backend's own xmin is held back by a transaction
# in another database.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# No Autovacuum, no random interferences.
$node->append_conf(
	'postgresql.conf', qq[
autovacuum = off
]);
$node->start;

# Create two databases.
$node->safe_psql('postgres', 'CREATE DATABASE db_holder');
$node->safe_psql('postgres', 'CREATE DATABASE db_target');

# Set up a table with TOAST data.  Returns the toast table name.
sub setup_table
{
	my ($tblname, $extra_ddl) = @_;
	$extra_ddl //= '';

	$node->safe_psql(
		'db_target', qq{
		DROP TABLE IF EXISTS $tblname;
		CREATE TABLE $tblname (id int PRIMARY KEY, data text)
			WITH (autovacuum_enabled = off);
		ALTER TABLE $tblname ALTER COLUMN data SET STORAGE EXTERNAL;
		INSERT INTO $tblname VALUES (1, repeat('x', 2500));
		INSERT INTO $tblname VALUES (2, repeat('z', 2500));
		$extra_ddl
	});

	my $toast = $node->safe_psql(
		'db_target', qq{
		SELECT 'pg_toast.' || relname FROM pg_class
		WHERE oid = (SELECT reltoastrelid FROM pg_class
					 WHERE relname = '$tblname');
	});
	chomp $toast;
	return $toast;
}

# Create precondition state.  Requires that $holder is already running with
# a write XID.
sub create_missing_toast_state
{
	my ($tblname, $toast_table) = @_;

	# Hold xmin in same DB to prevent main tuple removal.
	my $xmin_holder = $node->background_psql('db_target', on_error_stop => 1);
	$xmin_holder->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ");
	$xmin_holder->query_safe("SELECT 1");

	# Delete the row with TOAST data.
	$node->safe_psql('db_target', "DELETE FROM $tblname WHERE id = 1");

	# VACUUM main table only.  The xmin_holder keeps the dead tuple.
	$node->safe_psql('db_target', "VACUUM (PROCESS_TOAST false) $tblname");

	# Release same-DB xmin.
	$xmin_holder->query_safe("COMMIT");
	$xmin_holder->quit;

	# Vacuum toast table: toast chunks now DEAD and removed.
	$node->safe_psql('db_target', "VACUUM $toast_table");
}

# Open a transaction holding an XID before any deletes.  This XID will hold
# back GetSnapshotData() globally, on a different database than the one
# running the various rewrite operations.
my $holder = $node->background_psql('db_holder', on_error_stop => 1);
$holder->query_safe("BEGIN");
$holder->query_safe("CREATE TEMP TABLE xid_holder(x int)");

# Test 1: CLUSTER
my $toast = setup_table('rewrite_test');
create_missing_toast_state('rewrite_test', $toast);

my ($ret, $stdout, $stderr) =
  $node->psql('db_target', 'CLUSTER rewrite_test USING rewrite_test_pkey');
is($ret, 0, "CLUSTER succeeds without missing-chunk error");
unlike($stderr, qr/missing chunk/, "CLUSTER: no missing-chunk error");

# Test 2: VACUUM FULL
$toast = setup_table('rewrite_test');
create_missing_toast_state('rewrite_test', $toast);

($ret, $stdout, $stderr) =
  $node->psql('db_target', 'VACUUM FULL rewrite_test');
is($ret, 0, "VACUUM FULL succeeds without missing-chunk error");
unlike($stderr, qr/missing chunk/, "VACUUM FULL: no missing-chunk error");

# Test 3: REPACK
$toast = setup_table('rewrite_test');
create_missing_toast_state('rewrite_test', $toast);

($ret, $stdout, $stderr) = $node->psql('db_target', 'REPACK rewrite_test');
is($ret, 0, "REPACK succeeds without missing-chunk error");
unlike($stderr, qr/missing chunk/, "REPACK: no missing-chunk error");

# Test 4: CREATE INDEX on toasted column
$toast = setup_table('rewrite_test');
create_missing_toast_state('rewrite_test', $toast);

($ret, $stdout, $stderr) = $node->psql('db_target',
	'CREATE INDEX rewrite_test_data_idx ON rewrite_test ((rewrite_test IS NOT NULL))');
is($ret, 0, "CREATE INDEX succeeds without missing-chunk error");
unlike($stderr, qr/missing chunk/, "CREATE INDEX: no missing-chunk error");

# Cleanup.
$holder->query_safe("COMMIT");
$holder->quit;

$node->stop;
done_testing();
