# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Demonstrates a documented residual gap in the conflict log table's
# per-column size cap (CONFLICT_MAX_VALUE_SIZE in conflict.c): the cap
# bounds a value's *stored* size before deciding whether to render it into
# the log, but never bounds what the type's own output function actually
# returns.  A C-language type that stores a few bytes, sends/receives just
# as few, but whose output function ignores its input and always renders
# hundreds of megabytes, sails through the cap untouched.
#
# This is not something the patch is expected to catch (see the "XXX" note
# above the cap in build_index_key_json()) -- reaching it requires a
# C-language output function and a binary=true subscription, well outside
# what an ordinary subscription owner can trigger with SQL alone.  This
# test exists to make that documented gap concrete, not to assert a fix.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

for my $node ($node_publisher, $node_subscriber)
{
	$node->safe_psql('postgres', 'CREATE EXTENSION test_conflict_typeout');
	$node->safe_psql('postgres',
		"CREATE TABLE bt (a int, b boomtype, c boomtype, PRIMARY KEY (b, c))"
	);
}

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_bt FOR TABLE bt");

my $appname = 'sub_bt';
$node_subscriber->safe_psql(
	'postgres',
	"CREATE SUBSCRIPTION sub_bt
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION pub_bt WITH (binary = true, conflict_log_destination = all)"
);

$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Only 4 bytes per column cross the wire either way (boomtype_send), so
# this succeeds regardless of the pathological output function.
$node_publisher->safe_psql('postgres', "INSERT INTO bt VALUES (1, '100', '200')");
$node_publisher->wait_for_catchup($appname);

is( $node_subscriber->safe_psql('postgres', 'SELECT a FROM bt'),
	'1',
	'row replicated using the compact binary send/recv, not the output function'
);

# Delete the row only on the subscriber, so the publisher's later UPDATE
# becomes an update_missing conflict -- a LOG-level conflict that must
# render the (b, c) replica identity key into the conflict log table.
$node_subscriber->safe_psql('postgres', 'DELETE FROM bt');

my $log_offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"UPDATE bt SET a = 2 WHERE b = '100' AND c = '200'");

# Rendering both (b, c) values via boomtype_out produces 600MB each,
# 1.2GB combined -- past the ~1GB StringInfo limit -- even though the
# per-column size cap saw only the 8-byte stored form of each and let
# both through unomitted.
$node_subscriber->wait_for_log(
	qr/string buffer exceeds maximum allowed length/,
	$log_offset);

pass(
	'oversized user-defined typeoutput overflows conflict log table rendering, '
	  . 'despite each value passing the per-column size cap'
);

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
