# Copyright (c) 2026, PostgreSQL Global Development Group

# ALTER SUBSCRIPTION ... REFRESH TABLE refuses to run when another
# subscription also feeds the table, because the truncate would discard rows
# that the other subscription maintains but would never copy again.  A
# subscription that feeds the table by routing rows through a partitioned
# ancestor is one of those, and the ancestor is not itself truncated, so it
# only counts if the command locks it.  Check that a subscription cannot be
# registered on such an ancestor while the command runs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# The leaf is published on its own, and the root is published with
# publish_via_partition_root, so a subscription to either one keeps the leaf
# populated.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_race (a int primary key) PARTITION BY RANGE (a);
	CREATE TABLE tab_race_1 PARTITION OF tab_race FOR VALUES FROM (1) TO (12);
	INSERT INTO tab_race SELECT generate_series(1, 10);
	CREATE PUBLICATION tap_pub_race_leaf FOR TABLE tab_race_1;
	CREATE PUBLICATION tap_pub_race_root FOR TABLE tab_race
		WITH (publish_via_partition_root = true);
));

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE EXTENSION injection_points;
	CREATE TABLE tab_race (a int primary key) PARTITION BY RANGE (a);
	CREATE TABLE tab_race_1 PARTITION OF tab_race FOR VALUES FROM (1) TO (12);
	CREATE SUBSCRIPTION tap_sub_race_leaf CONNECTION '$publisher_connstr'
		PUBLICATION tap_pub_race_leaf;
));
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_race_leaf');

is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_race_1"),
	'10', 'leaf subscription copied the baseline');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_race_leaf DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub_race_leaf' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

# Pause the command once it has taken its locks and cleared its final check.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('subscription-refresh-table-after-subscription-check', 'wait')"
);

my $refresh = $node_subscriber->background_psql('postgres');
$refresh->query_until(
	qr/starting_refresh/, q(
	\echo starting_refresh
	ALTER SUBSCRIPTION tap_sub_race_leaf REFRESH TABLE tab_race_1;
));
$node_subscriber->wait_for_event('client backend',
	'subscription-refresh-table-after-subscription-check');

# Registering a subscription resolves the relation with AccessShareLock first,
# so this has to wait for the refresh to release the root.  Run it in the
# background, since it is expected not to return yet.
my $create = $node_subscriber->background_psql('postgres');
$create->query_until(
	qr/starting_create/, qq(
	\\echo starting_create
	CREATE SUBSCRIPTION tap_sub_race_root CONNECTION '$publisher_connstr'
		PUBLICATION tap_pub_race_root WITH (copy_data = false, enabled = false);
));

ok( $node_subscriber->poll_query_until(
		'postgres', qq(
	SELECT count(*) > 0 FROM pg_locks
	  WHERE relation = 'tab_race'::regclass AND NOT granted
)), 'CREATE SUBSCRIPTION waits for the lock on the partitioned ancestor');

is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_subscription WHERE subname = 'tap_sub_race_root'"
	),
	'0',
	'the ancestor subscription is not registered while the refresh holds the lock'
);

# Let the refresh finish.  The waiting CREATE SUBSCRIPTION can then proceed,
# and it sees a table that has already been reset.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_wakeup('subscription-refresh-table-after-subscription-check')"
);
ok($refresh->quit, 'refresh completed');
ok($create->quit, 'CREATE SUBSCRIPTION completed once the lock was released');

is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT r.srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 'tap_sub_race_leaf'"
	),
	'i',
	'the refreshed leaf is reset for re-copy');

# The refresh went through because it was alone when it checked and stayed
# alone until it committed.  Re-running it now is rejected, because the
# subscription on the ancestor is visible and would lose rows.
my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_race_leaf REFRESH TABLE tab_race_1");
isnt($ret, 0, 'a later refresh is rejected');
like(
	$stderr,
	qr/is a partition of "tab_race", which is part of the subscription "tap_sub_race_root"/,
	'rejected because the ancestor subscription now feeds the leaf');

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_race_leaf ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_race_leaf');
is($node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_race_1"),
	'10', 'the leaf is refilled by its own subscription');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
