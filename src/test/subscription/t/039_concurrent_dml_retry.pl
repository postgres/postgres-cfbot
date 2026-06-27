# Copyright (c) 2026, PostgreSQL Global Development Group

# Test concurrent DML retry paths in logical replication apply.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher.
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create tables on both sides and set up replication.
$node_publisher->safe_psql('postgres', qq(
	CREATE TABLE test_tab (a int PRIMARY KEY, b int, c text);
	CREATE TABLE test_tab_full (a int, b int, c text);
	ALTER TABLE test_tab_full REPLICA IDENTITY FULL;
));

$node_subscriber->safe_psql('postgres', qq(
	CREATE TABLE test_tab (a int PRIMARY KEY, b int, c text);
	CREATE TABLE test_tab_full (a int, b int, c text);
	ALTER TABLE test_tab_full REPLICA IDENTITY FULL;
));

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION test_pub FOR TABLE test_tab, test_tab_full;");

my $appname = 'test_sub';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION test_sub
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION test_pub;");

$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Insert test data.  Rows 1-2 are used by XactLockTableWait tests,
# rows 3-4 by injection-point tests.
$node_publisher->safe_psql('postgres', qq(
	INSERT INTO test_tab VALUES (1, 10, 'foo'), (2, 20, 'bar'),
	                            (3, 30, 'baz'), (4, 40, 'qux');
	INSERT INTO test_tab_full VALUES (1, 10, 'foo'), (2, 20, 'bar'),
	                                 (3, 30, 'baz'), (4, 40, 'qux');
));
$node_publisher->wait_for_catchup($appname);

# Test the apply worker retry when the dirty snapshot finds an in-progress
# transaction on the target tuple, causing XactLockTableWait.

my $sub_session = $node_subscriber->background_psql('postgres');

sub test_retry_with_in_progress_xact
{
	my (%args) = @_;
	my $test_name     = $args{name};
	my $dml           = $args{dml};
	my $verify_query  = $args{verify_query};
	my $verify_result = $args{verify_result};

	# Modify the target tuple in an in-progress transaction on the subscriber.
	$sub_session->query_safe("BEGIN;");
	$sub_session->query_safe($dml);

	# Run DML on the publisher.
	$node_publisher->safe_psql('postgres', $dml);

	# Wait for the apply worker to block on XactLockTableWait.
	$node_subscriber->poll_query_until('postgres', qq(
		SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'logical replication apply worker'
		  AND wait_event_type = 'Lock'
		  AND wait_event = 'transactionid';
	)) or die "Timed out waiting for apply worker to block on XactLockTableWait";

	# Abort so the apply worker wakes up, retries, and applies successfully.
	$sub_session->query_safe("ROLLBACK;");
	$node_publisher->wait_for_catchup($appname);

	# Verify the results.
	my $result = $node_subscriber->safe_psql('postgres', $verify_query);
	is($result, $verify_result, $test_name);
}

# Index scan (PK): publisher UPDATE with in-progress subscriber transaction.
test_retry_with_in_progress_xact(
	name          => 'XactLockTableWait index scan UPDATE',
	dml           => "UPDATE test_tab SET c = 'foo_u' WHERE a = 1;",
	verify_query  => "SELECT c FROM test_tab WHERE a = 1;",
	verify_result => 'foo_u',
);

# Index scan (PK): publisher DELETE with in-progress subscriber transaction.
test_retry_with_in_progress_xact(
	name          => 'XactLockTableWait index scan DELETE',
	dml           => "DELETE FROM test_tab WHERE a = 2;",
	verify_query  => "SELECT count(*) FROM test_tab WHERE a = 2;",
	verify_result => '0',
);

# Seq scan (REPLICA IDENTITY FULL): publisher UPDATE with in-progress
# subscriber transaction.
test_retry_with_in_progress_xact(
	name          => 'XactLockTableWait seq scan UPDATE',
	dml           => "UPDATE test_tab_full SET c = 'foo_u' WHERE a = 1;",
	verify_query  => "SELECT c FROM test_tab_full WHERE a = 1;",
	verify_result => 'foo_u',
);

# Seq scan (REPLICA IDENTITY FULL): publisher DELETE with in-progress
# subscriber transaction.
test_retry_with_in_progress_xact(
	name          => 'XactLockTableWait seq scan DELETE',
	dml           => "DELETE FROM test_tab_full WHERE a = 2;",
	verify_query  => "SELECT count(*) FROM test_tab_full WHERE a = 2;",
	verify_result => '0',
);

$sub_session->quit;

# Test the apply worker retry when table_tuple_lock detects a concurrently
# updated or deleted tuple (TM_Updated / TM_Deleted). An injection point pauses
# the worker between finding the tuple and locking it, allowing concurrent DML
# to intervene.

sub test_retry_with_concurrent_dml_before_tuple_lock
{
	my (%args) = @_;
	my $test_name     = $args{name};
	my $inj_point     = $args{inj_point};
	my $dml           = $args{dml};
	my $expected_log  = $args{expected_log};
	my $verify_query  = $args{verify_query};
	my $verify_result = $args{verify_result};

	$node_subscriber->safe_psql('postgres',
		"SELECT injection_points_attach('$inj_point', 'wait');");

	# Run DML on the publisher.
	$node_publisher->safe_psql('postgres', $dml);

	$node_subscriber->wait_for_event('logical replication apply worker',
		$inj_point);

	# Run DML on the subscriber.
	$node_subscriber->safe_psql('postgres', $dml);

	my $log_offset = -s $node_subscriber->logfile;

	# Detach before wakeup so the retry doesn't hit the same injection point.
	$node_subscriber->safe_psql('postgres',
		"SELECT injection_points_detach('$inj_point');
		 SELECT injection_points_wakeup('$inj_point');");

	$node_subscriber->wait_for_log($expected_log, $log_offset);
	pass("$test_name: concurrent modification detected and retried");

	$node_publisher->wait_for_catchup($appname);

	my $result = $node_subscriber->safe_psql('postgres', $verify_query);
	is($result, $verify_result, "$test_name: data correct after retry");
}

# Check whether injection_points extension is available on the subscriber.
my $injection_points_supported =
	$node_subscriber->check_extension('injection_points');

if ($injection_points_supported != 0)
{
	$node_subscriber->safe_psql('postgres',
		"CREATE EXTENSION injection_points;");

	# TM_Updated via index scan (PK).
	test_retry_with_concurrent_dml_before_tuple_lock(
		name          => 'index scan TM_Updated',
		inj_point     => 'find-repl-tuple-by-index-before-lock',
		dml           => "UPDATE test_tab SET c = 'baz_u' WHERE a = 3;",
		expected_log  => qr/concurrent update, retrying/,
		verify_query  => "SELECT c FROM test_tab WHERE a = 3;",
		verify_result => 'baz_u',
	);

	# TM_Deleted via index scan (PK).
	test_retry_with_concurrent_dml_before_tuple_lock(
		name          => 'index scan TM_Deleted',
		inj_point     => 'find-repl-tuple-by-index-before-lock',
		dml           => "DELETE FROM test_tab WHERE a = 4;",
		expected_log  => qr/concurrent delete, retrying/,
		verify_query  => "SELECT count(*) FROM test_tab WHERE a = 4;",
		verify_result => '0',
	);

	# TM_Updated via seq scan (REPLICA IDENTITY FULL).
	test_retry_with_concurrent_dml_before_tuple_lock(
		name          => 'seq scan TM_Updated',
		inj_point     => 'find-repl-tuple-seq-before-lock',
		dml           => "UPDATE test_tab_full SET c = 'baz_u' WHERE a = 3;",
		expected_log  => qr/concurrent update, retrying/,
		verify_query  => "SELECT c FROM test_tab_full WHERE a = 3;",
		verify_result => 'baz_u',
	);

	# TM_Deleted via seq scan (REPLICA IDENTITY FULL).
	test_retry_with_concurrent_dml_before_tuple_lock(
		name          => 'seq scan TM_Deleted',
		inj_point     => 'find-repl-tuple-seq-before-lock',
		dml           => "DELETE FROM test_tab_full WHERE a = 4;",
		expected_log  => qr/concurrent delete, retrying/,
		verify_query  => "SELECT count(*) FROM test_tab_full WHERE a = 4;",
		verify_result => '0',
	);
}

done_testing();
