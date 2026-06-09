
# Copyright (c) 2026, PostgreSQL Global Development Group

# This tests that the target of logical replication cannot be global temporary
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create a publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create a subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create tables on publisher
$node_publisher->safe_psql('postgres', qq(
	CREATE TABLE perm_test (a int);
	CREATE TABLE gtt_test (a int);
	INSERT INTO perm_test VALUES (1);
	INSERT INTO gtt_test VALUES (1);
));

# Create same tables on subscriber, except make gtt_test global temporary
$node_subscriber->safe_psql('postgres', qq(
	CREATE TABLE perm_test (a int);
	CREATE GLOBAL TEMP TABLE gtt_test (a int);
));

# Setup logical replication on publisher
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', qq(
	CREATE PUBLICATION regress_perm_pub FOR TABLE perm_test;
	CREATE PUBLICATION regress_gtt_pub FOR TABLE gtt_test;
));

# Setup logical replication for GTT on subscriber -- should fail
my ($ret, $stdout, $stderr) =
	$node_subscriber->psql('postgres', qq(
		CREATE SUBSCRIPTION regress_sub
			CONNECTION '$publisher_connstr' PUBLICATION regress_gtt_pub;
));
like(
	$stderr,
	qr/ERROR:  cannot use relation "public\.gtt_test" as logical replication target
.*DETAIL:  This operation is not supported for global temporary relations\./,
	"could not use global temporary table as subscriber");

# Setup logical replication for permanent table -- OK
$node_subscriber->safe_psql('postgres', qq(
	CREATE SUBSCRIPTION regress_sub
		CONNECTION '$publisher_connstr' PUBLICATION regress_perm_pub
));

# Alter subscription to use GTT -- should fail
($ret, $stdout, $stderr) =
	$node_subscriber->psql('postgres',
		"ALTER SUBSCRIPTION regress_sub SET PUBLICATION regress_gtt_pub;");
like(
	$stderr,
	qr/ERROR:  cannot use relation "public\.gtt_test" as logical replication target
.*DETAIL:  This operation is not supported for global temporary relations\./,
	"could not use global temporary table as subscriber");

# Replace the subscriber table with a permanent one and try again
$node_subscriber->safe_psql('postgres', qq(
	DROP TABLE gtt_test;
	CREATE TABLE gtt_test (a int);
	ALTER SUBSCRIPTION regress_sub SET PUBLICATION regress_gtt_pub;
));

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'regress_sub');

# Replace the subscriber table with a global temporary table again
$node_subscriber->safe_psql('postgres', qq(
	DROP TABLE gtt_test;
	CREATE GLOBAL TEMP TABLE gtt_test (a int);
));

# Insert another row in the publisher table
my $offset = -s $node_subscriber->logfile;
$node_publisher->safe_psql('postgres',
	"INSERT INTO gtt_test VALUES (2)");

# Verify that an error is logged
$offset = $node_subscriber->wait_for_log(
	qr/ERROR:  cannot use relation "public\.gtt_test" as logical replication target
.*DETAIL:  This operation is not supported for global temporary relations\./,
	$offset);

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
