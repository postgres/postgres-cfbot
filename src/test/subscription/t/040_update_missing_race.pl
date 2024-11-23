# Copyright (c) 2025, PostgreSQL Global Development Group

# Test the conflict detection and resolution in logical replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

############################## Set it to 0 to make set success; TODO: delete that for commit
my $simulate_race_condition = 1;
##############################

###############################
# Setup
###############################

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	qq(track_commit_timestamp = on));
$node_publisher->start;


# Create subscriber node with track_commit_timestamp enabled
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf',
	qq(track_commit_timestamp = on));
$node_subscriber->start;


# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node_subscriber->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# Create table on publisher
$node_publisher->safe_psql(
	'postgres',
	"CREATE TABLE conf_tab(a int PRIMARY key, data text);");

# Create similar table on subscriber with additional index to disable HOT updates and additional column
$node_subscriber->safe_psql(
	'postgres',
	"CREATE TABLE conf_tab(a int PRIMARY key, data text, i int DEFAULT 0);
	 CREATE INDEX i_index ON conf_tab(i);");

# Set up extension to simulate race condition
$node_subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE conf_tab");

# Insert row to be updated later
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab(a, data) VALUES (1,'frompub')");

# Create the subscription
my $appname = 'tap_sub';
$node_subscriber->safe_psql(
	'postgres',
	"CREATE SUBSCRIPTION tap_sub
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION tap_pub");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

############################################
# Race condition because of DirtySnapshot
############################################

my $psql_session_subscriber = $node_subscriber->background_psql('postgres');
if ($simulate_race_condition)
{
	$node_subscriber->safe_psql('postgres', "SELECT injection_points_attach('index_getnext_slot_before_fetch_apply_dirty', 'wait')");
}

my $log_offset = -s $node_subscriber->logfile;

# Update tuple on publisher
$node_publisher->safe_psql('postgres',
	"UPDATE conf_tab SET data = 'frompubnew' WHERE (a=1);");


if ($simulate_race_condition)
{
	# Wait apply worker to start the search for the tuple using index
	$node_subscriber->wait_for_event('logical replication apply worker', 'index_getnext_slot_before_fetch_apply_dirty');
}

# Update additional(!) column on the subscriber
$psql_session_subscriber->query_until(
	qr/start/, qq[
	\\echo start
	UPDATE conf_tab SET i = 1 WHERE (a=1);
]);


if ($simulate_race_condition)
{
	# Wake up apply worker
	$node_subscriber->safe_psql('postgres',"
		SELECT injection_points_detach('index_getnext_slot_before_fetch_apply_dirty');
		SELECT injection_points_wakeup('index_getnext_slot_before_fetch_apply_dirty');
		");
}

# Tuple was updated - so, we have conflict
$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab\"/,
	$log_offset);

$node_publisher->wait_for_catchup($appname);

# We need new column value be synced with subscriber
is($node_subscriber->safe_psql('postgres', 'SELECT data from conf_tab WHERE a = 1'), 'frompubnew', 'record updated on subscriber');
# And additional column maintain updated value
is($node_subscriber->safe_psql('postgres', 'SELECT i from conf_tab WHERE a = 1'), 1, 'column record updated on subscriber');

ok(!$node_subscriber->log_contains(
		qr/LOG:  conflict detected on relation \"public.conf_tab\": conflict=update_missing/,
		$log_offset), 'invalid conflict detected');

ok($node_subscriber->log_contains(
		qr/LOG:  conflict detected on relation \"public.conf_tab\": conflict=update_origin_differs/,
		$log_offset), 'correct conflict detected');

done_testing();
