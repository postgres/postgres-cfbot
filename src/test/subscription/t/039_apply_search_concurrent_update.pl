# Copyright (c) 2026, PostgreSQL Global Development Group

# Apply searching for a row that is updated concurrently on the subscriber.
#
# The apply worker looks up the row to update or delete through the replica
# identity index.  A non-HOT update of that row on the subscriber gives it a
# new index entry.  If that update commits after the index has returned the
# old version's TID but before apply fetches it, the old version is dead by
# then and the new entry may lie where the scan has already been.  A search
# that sees the row in neither version takes it for missing and skips the
# change, although the row existed all along.  Apply must find the row and
# report the actual conflict: a row modified by another origin.

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
$node_publisher->append_conf('postgresql.conf',
	'track_commit_timestamp = on');
$node_publisher->start;

# track_commit_timestamp is needed to detect rows modified by another
# origin, and wal_level = replica for retain_dead_tuples.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf(
	'postgresql.conf', qq(
track_commit_timestamp = on
wal_level = replica
));
$node_subscriber->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node_subscriber->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node_subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# The subscriber's table has a column of its own, indexed so that updating
# it is never HOT.
$node_publisher->safe_psql('postgres',
	'CREATE TABLE conf_tab (a int PRIMARY KEY, data text)');
$node_subscriber->safe_psql(
	'postgres', q[
	CREATE TABLE conf_tab (a int PRIMARY KEY, data text, i int DEFAULT 0);
	CREATE INDEX i_index ON conf_tab (i);
]);

$node_publisher->safe_psql(
	'postgres', q[
	INSERT INTO conf_tab VALUES (1, 'frompub'), (2, 'frompub'), (3, 'frompub');
	CREATE PUBLICATION tap_pub FOR TABLE conf_tab;
]);

my $appname = 'tap_sub';
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Run $pub_sql on the publisher.  Hold apply after the index has returned
# the row's TID and before the heap fetch, run $sub_sql on the subscriber
# meanwhile, then let apply go on.  Return the conflicts apply reported.
sub apply_during_local_update
{
	my ($pub_sql, $sub_sql) = @_;
	my $point = 'index_getnext_slot_before_fetch_apply_dirty';
	my $log_offset = -s $node_subscriber->logfile;

	$node_subscriber->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	$node_publisher->safe_psql('postgres', $pub_sql);
	$node_subscriber->wait_for_event('logical replication apply worker',
		$point);

	$node_subscriber->safe_psql('postgres', $sub_sql);

	$node_subscriber->safe_psql(
		'postgres', qq[
		SELECT injection_points_detach('$point');
		SELECT injection_points_wakeup('$point');
	]);

	$node_subscriber->wait_for_log(
		qr/conflict detected on relation "public.conf_tab"/, $log_offset);
	$node_publisher->wait_for_catchup($appname);

	my $log = slurp_file($node_subscriber->logfile, $log_offset);
	return join(' ',
		$log =~
		  /conflict detected on relation "public\.conf_tab": conflict=(\w+)/g
	);
}

# DELETE on the publisher, not delete_missing.
is( apply_during_local_update(
		'DELETE FROM conf_tab WHERE a = 1',
		'UPDATE conf_tab SET i = 1 WHERE a = 1'),
	'delete_origin_differs',
	'DELETE: conflict with the locally updated row');
is( $node_subscriber->safe_psql(
		'postgres', 'SELECT count(*) FROM conf_tab WHERE a = 1'),
	'0',
	'DELETE: row deleted on subscriber');

# UPDATE on the publisher, not update_missing.  The local update touches a
# column the publisher does not have, so both updates must survive.
is( apply_during_local_update(
		"UPDATE conf_tab SET data = 'frompubnew' WHERE a = 2",
		'UPDATE conf_tab SET i = 1 WHERE a = 2'),
	'update_origin_differs',
	'UPDATE: conflict with the locally updated row');
is( $node_subscriber->safe_psql(
		'postgres', 'SELECT data, i FROM conf_tab WHERE a = 2'),
	'frompubnew|1',
	'UPDATE: both updates applied');

# The same with retain_dead_tuples, where a row taken for missing is looked
# for among the dead ones and found there: not update_deleted.
$node_subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION tap_sub DISABLE');
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION tap_sub SET (retain_dead_tuples = true)');
$node_subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION tap_sub ENABLE');
ok( $node_subscriber->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	'dead tuples are retained');

is( apply_during_local_update(
		"UPDATE conf_tab SET data = 'frompubnew' WHERE a = 3",
		'UPDATE conf_tab SET i = 1 WHERE a = 3'),
	'update_origin_differs',
	'UPDATE with retain_dead_tuples: conflict with the locally updated row');
is( $node_subscriber->safe_psql(
		'postgres', 'SELECT data, i FROM conf_tab WHERE a = 3'),
	'frompubnew|1',
	'UPDATE with retain_dead_tuples: both updates applied');

done_testing();
