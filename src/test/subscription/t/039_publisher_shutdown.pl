# Copyright (c) 2026, PostgreSQL Global Development Group

# Clean publisher shutdown must not ERROR in endstreaming or disable a
# disable_on_error subscription. Replication continues after restart.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_tab (id int PRIMARY KEY);
	INSERT INTO test_tab VALUES (1);
	CREATE PUBLICATION test_pub FOR TABLE test_tab;
));

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_tab (id int PRIMARY KEY);
	CREATE SUBSCRIPTION test_sub
		CONNECTION '$publisher_connstr'
		PUBLICATION test_pub
		WITH (disable_on_error = true);
));

$node_subscriber->wait_for_subscription_sync($node_publisher, 'test_sub');

my $log_offset = -s $node_subscriber->logfile;
$node_publisher->stop;

# Wait until a successor apply worker fails to connect. That means the
# previous process finished endstreaming with the subscription still enabled.
$node_subscriber->wait_for_log(
	qr/apply worker for subscription "test_sub" could not connect to the publisher/,
	$log_offset);

ok( !$node_subscriber->log_contains(
		'could not send end-of-streaming message', $log_offset),
	'apply worker does not fail ending the stream after publisher shutdown');
ok( !$node_subscriber->log_contains(
		'subscription "test_sub" has been disabled because of an error',
		$log_offset),
	'disable_on_error subscription is not disabled on clean publisher shutdown');

my $enabled = $node_subscriber->safe_psql('postgres',
	"SELECT subenabled FROM pg_subscription WHERE subname = 'test_sub'");
is($enabled, 't', 'subscription remains enabled');

$node_publisher->start;
$node_publisher->safe_psql('postgres', "INSERT INTO test_tab VALUES (2)");
$node_publisher->wait_for_catchup('test_sub');

my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM test_tab");
is($result, '2', 'subscription resumes after publisher restart');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
