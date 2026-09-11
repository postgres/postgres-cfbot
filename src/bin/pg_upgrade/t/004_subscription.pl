# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# Test for pg_upgrade of logical subscription. Note that after the successful
# upgrade test, we can't use the old cluster to prevent failing in --link mode.
use strict;
use warnings FATAL => 'all';

use File::Find qw(find);
use File::Path qw(rmtree);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize publisher node
my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

# Initialize the old subscriber node
my $old_sub = PostgreSQL::Test::Cluster->new('old_sub');
$old_sub->init(allows_streaming => 'physical');
$old_sub->start;
my $oldbindir = $old_sub->config_data('--bindir');

# Initialize the new subscriber
my $new_sub = PostgreSQL::Test::Cluster->new('new_sub');
$new_sub->init(allows_streaming => 'physical');
my $newbindir = $new_sub->config_data('--bindir');

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

# Remember a connection string for the publisher node. It would be used
# several times.
my $connstr = $publisher->connstr . ' dbname=postgres';

# ------------------------------------------------------
# Check that pg_upgrade fails when max_active_replication_origins configured
# in the new cluster is less than the number of active replication origins in
# the old cluster.
# ------------------------------------------------------
$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub1");
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub1 CONNECTION '$connstr' PUBLICATION regress_pub1"
);

# Wait until the apply worker sets up the replication origin, making it
# active. Otherwise, stopping the old cluster too early could leave the
# origin inactive, causing the check below to see no active replication
# origins and fail.
$old_sub->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_replication_origin_status")
  or die "Timed out while waiting for the replication origin to become active";

$old_sub->stop;

$new_sub->append_conf('postgresql.conf',
	"max_active_replication_origins = 0");

# pg_upgrade will fail because the new cluster has insufficient
# max_active_replication_origins.
command_checks_all(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode,
		'--check',
	],
	1,
	[
		qr/"max_active_replication_origins" \(0\) must be greater than or equal to the number of active replication origins \(1\) in the old cluster/
	],
	[qr//],
	'run of pg_upgrade where the new cluster has insufficient max_active_replication_origins'
);

# Reset max_active_replication_origins
$new_sub->append_conf('postgresql.conf',
	"max_active_replication_origins = 10");

# Cleanup
$publisher->safe_psql('postgres', "DROP PUBLICATION regress_pub1");
$old_sub->start;
$old_sub->safe_psql('postgres', "DROP SUBSCRIPTION regress_sub1;");

# ------------------------------------------------------
# Check that pg_upgrade fails when max_replication_slots configured in the new
# cluster is less than the number of logical slots in the old cluster + 1 when
# subscription's retain_dead_tuples option is enabled.
# ------------------------------------------------------
# It is sufficient to use disabled subscription to test upgrade failure.

$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub1");
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub1 CONNECTION '$connstr' PUBLICATION regress_pub1 WITH (enabled = false, retain_dead_tuples = true)"
);

$old_sub->stop;

$new_sub->append_conf('postgresql.conf', 'max_replication_slots = 0');

# pg_upgrade will fail because the new cluster has insufficient
# max_replication_slots.
command_checks_all(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode,
		'--check',
	],
	1,
	[
		qr/"max_replication_slots" \(0\) must be greater than or equal to the number of logical replication slots in the old cluster plus one additional slot required for retaining conflict detection information \(1\)/
	],
	[qr//],
	'run of pg_upgrade where the new cluster has insufficient max_replication_slots'
);

# Reset max_replication_slots
$new_sub->append_conf('postgresql.conf', 'max_replication_slots = 10');

# Cleanup
$publisher->safe_psql('postgres', "DROP PUBLICATION regress_pub1");
$old_sub->start;
$old_sub->safe_psql('postgres', "DROP SUBSCRIPTION regress_sub1;");

# ------------------------------------------------------
# Check that pg_upgrade refuses to run if:
# a) there's a subscription with tables in a state other than 'r' (ready) or
#    'i' (init) and/or
# b) the subscription has no replication origin.
# ------------------------------------------------------
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_primary_key(id serial PRIMARY KEY);
		INSERT INTO tab_primary_key values(1);
		CREATE PUBLICATION regress_pub2 FOR TABLE tab_primary_key;
]);

# Insert the same value that is already present in publisher to the primary key
# column of subscriber so that the table sync will fail.
$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_primary_key(id serial PRIMARY KEY);
		INSERT INTO tab_primary_key values(1);
		CREATE SUBSCRIPTION regress_sub2 CONNECTION '$connstr' PUBLICATION regress_pub2;
]);

# Table will be in 'd' (data is being copied) state as table sync will fail
# because of primary key constraint error.
my $started_query =
  "SELECT count(1) = 1 FROM pg_subscription_rel WHERE srsubstate = 'd'";
$old_sub->poll_query_until('postgres', $started_query)
  or die
  "Timed out while waiting for the table state to become 'd' (datasync)";

# Setup another logical replication and drop the subscription's replication
# origin.
$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub3");
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub3 CONNECTION '$connstr' PUBLICATION regress_pub3 WITH (enabled = false)"
);
my $sub_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_subscription WHERE subname = 'regress_sub3'");
my $replorigin = 'pg_' . qq($sub_oid);
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_drop('$replorigin')");

$old_sub->stop;

command_checks_all(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode,
		'--check',
	],
	1,
	[
		qr/\QYour installation contains subscriptions without origin or having relations not in i (initialize) or r (ready) state\E/
	],
	[],
	'run of pg_upgrade --check for old instance with relation in \'d\' datasync(invalid) state and missing replication origin'
);

# Verify the reason why the subscriber cannot be upgraded
my $sub_relstate_filename;

# Find a txt file that contains a list of tables that cannot be upgraded. We
# cannot predict the file's path because the output directory contains a
# milliseconds timestamp. File::Find::find must be used.
find(
	sub {
		if ($File::Find::name =~ m/subs_invalid\.txt/)
		{
			$sub_relstate_filename = $File::Find::name;
		}
	},
	$new_sub->data_dir . "/pg_upgrade_output.d");

# Check the file content which should have tab_primary_key table in an invalid
# state.
like(
	slurp_file($sub_relstate_filename),
	qr/The table sync state \"d\" is not allowed for database:\"postgres\" subscription:\"regress_sub2\" schema:\"public\" relation:\"tab_primary_key\"/m,
	'the previous test failed due to subscription table in invalid state');

# Check the file content which should have regress_sub3 subscription.
like(
	slurp_file($sub_relstate_filename),
	qr/The replication origin is missing for database:\"postgres\" subscription:\"regress_sub3\"/m,
	'the previous test failed due to missing replication origin');

# Cleanup
$old_sub->start;
$publisher->safe_psql(
	'postgres', qq[
		DROP PUBLICATION regress_pub2;
		DROP PUBLICATION regress_pub3;
		DROP TABLE tab_primary_key;
]);
$old_sub->safe_psql(
	'postgres', qq[
		DROP SUBSCRIPTION regress_sub2;
		DROP SUBSCRIPTION regress_sub3;
		DROP TABLE tab_primary_key;
]);
rmtree($new_sub->data_dir . "/pg_upgrade_output.d");

# Verify that the upgrade should be successful with tables in 'ready'/'init'
# state along with retaining the replication origin's remote lsn,
# subscription's running status, failover option, and retain_dead_tuples
# option. Use multiple tables to verify deterministic pg_dump ordering
# of subscription relations during --binary-upgrade.
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded(id int);
		CREATE TABLE tab_upgraded1(id int);
		CREATE PUBLICATION regress_pub4 FOR TABLE tab_upgraded, tab_upgraded1;
]);

$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded(id int);
		CREATE TABLE tab_upgraded1(id int);
		CREATE SUBSCRIPTION regress_sub4 CONNECTION '$connstr' PUBLICATION regress_pub4 WITH (failover = true, retain_dead_tuples = true);
]);

# Wait till the tables tab_upgraded and tab_upgraded1 reach 'ready' state
my $synced_query =
  "SELECT count(1) = 2 FROM pg_subscription_rel WHERE srsubstate = 'r'";
$old_sub->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for the table to reach ready state";

$publisher->safe_psql('postgres',
	"INSERT INTO tab_upgraded1 VALUES (generate_series(1,50))");
$publisher->wait_for_catchup('regress_sub4');

# Change configuration to prepare a subscription table in init state
$old_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 0");
$old_sub->restart;

# Setup another logical replication
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded2(id int);
		CREATE PUBLICATION regress_pub5 FOR TABLE tab_upgraded2;
]);
$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded2(id int);
		CREATE SUBSCRIPTION regress_sub5 CONNECTION '$connstr' PUBLICATION regress_pub5;
]);

# The table tab_upgraded2 will be in the init state as the subscriber's
# configuration for max_logical_replication_workers is set to 0.
my $result = $old_sub->safe_psql('postgres',
	"SELECT count(1) = 1 FROM pg_subscription_rel WHERE srsubstate = 'i'");
is($result, qq(t), "Check that the table is in init state");

# Get the replication origin's remote_lsn of the old subscriber
my $remote_lsn = $old_sub->safe_psql('postgres',
	"SELECT os.remote_lsn
     FROM pg_replication_origin_status os
     JOIN pg_replication_origin o ON o.roident = os.local_id
     JOIN pg_subscription s ON o.roname = 'pg_' || s.oid::text
     WHERE s.subname = 'regress_sub4'"
);

# Get the replication origin ids (roident) for all subscriptions, keyed by
# subscription name (which is stable across upgrade, unlike suboid). These
# must be preserved after upgrade. A mismatch would cause spurious
# update_origin_differs conflicts.
my %pre_upgrade_roident;
my $roident_rows = $old_sub->safe_psql('postgres',
	"SELECT s.subname, o.roident
     FROM pg_subscription s
     JOIN pg_replication_origin o ON o.roname = 'pg_' || s.oid::text
     ORDER BY s.subname"
);
for my $row (split /\n/, $roident_rows)
{
	my ($subname, $roident) = split /\|/, $row;
	$pre_upgrade_roident{$subname} = $roident;
}

# Create a user created replication origin, which should also be preserved after upgrade.
my $user_origin_name = 'regress_user_origin';
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_create('$user_origin_name')");
$pre_upgrade_roident{$user_origin_name} = $old_sub->safe_psql('postgres',
	"SELECT roident FROM pg_replication_origin WHERE roname = '$user_origin_name'"
);

# An origin advanced to exactly 0/0 is tracked on the old cluster and must stay
# tracked.  This is the case that the NULL-ness of the remote_lsn argument
# exists to catch: a missing status row and a status row at 0/0 both reach the
# new cluster as InvalidXLogRecPtr.
my $zero_origin_name = 'regress_zero_origin';
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_create('$zero_origin_name')");
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_advance('$zero_origin_name', '0/0')");

# Have the subscription in disabled state before upgrade
$old_sub->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub5 DISABLE");

my $tab_upgraded_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'tab_upgraded'");
my $tab_upgraded1_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'tab_upgraded1'");
my $tab_upgraded2_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'tab_upgraded2'");

$sub_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_subscription ORDER BY subname");

$old_sub->stop;

# Change configuration so that initial table sync does not get started
# automatically
$new_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 0");

# ------------------------------------------------------
# Check that pg_upgrade is successful when all tables are in ready or in
# init state (tab_upgraded and tab_upgraded1 tables are in ready state and
# tab_upgraded2 table is in init state) along with retaining the replication
# origin's remote lsn, subscription's running status, failover option, and
# retain_dead_tuples option.
# ------------------------------------------------------
command_ok(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode
	],
	'run of pg_upgrade for old instance when the subscription tables are in init/ready state'
);
ok( !-d $new_sub->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ removed after successful pg_upgrade");

# ------------------------------------------------------
# Check that the data inserted to the publisher when the new subscriber is down
# will be replicated once it is started. Also check that the old subscription
# states and relations origins are all preserved, and that the conflict
# detection slot is created. In addition, verify that the replication origin
# identifiers (roident) for both subscription-related and user-created
# replication origins are preserved after the upgrade.
# ------------------------------------------------------
$publisher->safe_psql(
	'postgres', qq[
		INSERT INTO tab_upgraded1 VALUES(51);
		INSERT INTO tab_upgraded2 VALUES(1);
]);

$new_sub->start;

# The subscription oid should be preserved
$result = $new_sub->safe_psql('postgres', "SELECT oid FROM pg_subscription ORDER BY subname");
is($result, qq($sub_oid), "subscription oid should have been preserved");

# The subscription's running status, failover option, and retain_dead_tuples
# option should be preserved in the upgraded instance. So regress_sub4 should
# still have subenabled, subfailover, and subretaindeadtuples set to true,
# while regress_sub5 should have both set to false.
$result = $new_sub->safe_psql('postgres',
	"SELECT subname, subenabled, subfailover, subretaindeadtuples FROM pg_subscription ORDER BY subname"
);
is( $result, qq(regress_sub4|t|t|t
regress_sub5|f|f|f),
	"check that the subscription's running status, failover, and retain_dead_tuples are preserved"
);

# Verify that subscription replication origins retain their origin IDs after
# upgrade.
my $post_roident_rows = $new_sub->safe_psql('postgres',
	"SELECT s.subname, o.roident
     FROM pg_subscription s
     JOIN pg_replication_origin o ON o.roname = 'pg_' || s.oid::text
     ORDER BY s.subname"
);

my %post_upgrade_roident;
for my $row (split /\n/, $post_roident_rows)
{
	my ($subname, $roident) = split /\|/, $row;
	$post_upgrade_roident{$subname} = $roident;
}

# Iterate over the union of pre- and post-upgrade keys so a missing or
# unexpected entry on either side is caught.
my %all_subnames = (%pre_upgrade_roident, %post_upgrade_roident);
for my $subname (sort keys %all_subnames)
{
	next if $subname eq $user_origin_name;
	is($post_upgrade_roident{$subname}, $pre_upgrade_roident{$subname},
		"roident preserved for subscription '$subname' after upgrade");
}

# Verify that the user created replication origin retains it's origin IDs after
# upgrade.
my $post_user_roident = $new_sub->safe_psql('postgres',
	"SELECT roident FROM pg_replication_origin WHERE roname = '$user_origin_name'"
);
is($post_user_roident, $pre_upgrade_roident{$user_origin_name},
	"roident preserved for user-created origin '$user_origin_name' after upgrade"
);

# Origins never advanced on the old cluster do not appear in
# pg_replication_origin_status on the new cluster.
$result = $new_sub->safe_psql(
	'postgres', qq[
	SELECT count(*) FROM pg_replication_origin_status s
	  JOIN pg_replication_origin o ON o.roident = s.local_id
	 WHERE o.roname = '$user_origin_name']);
is($result, qq(0),
	"never-advanced origin '$user_origin_name' is still untracked after upgrade"
);

# The origin advanced to 0/0 is still present in pg_replication_origin_status
# on the new cluster.
$result = $new_sub->safe_psql(
	'postgres', qq[
	SELECT count(*) FROM pg_replication_origin_status s
	  JOIN pg_replication_origin o ON o.roident = s.local_id
	 WHERE o.roname = '$zero_origin_name']);
is($result, qq(1),
	"origin '$zero_origin_name' advanced to 0/0 is still tracked after upgrade"
);

# Subscription relations should be preserved
$result = $new_sub->safe_psql('postgres',
	"SELECT srrelid, srsubstate FROM pg_subscription_rel ORDER BY srrelid");
is( $result, qq($tab_upgraded_oid|r
$tab_upgraded1_oid|r
$tab_upgraded2_oid|i),
	"there should be 3 rows in pg_subscription_rel(representing tab_upgraded, tab_upgraded1 and tab_upgraded2)"
);

# The replication origin's remote_lsn should be preserved
$sub_oid = $new_sub->safe_psql('postgres',
	"SELECT oid FROM pg_subscription WHERE subname = 'regress_sub4'");
$result = $new_sub->safe_psql('postgres',
	"SELECT remote_lsn FROM pg_replication_origin_status WHERE external_id = 'pg_' || $sub_oid"
);
is($result, qq($remote_lsn), "remote_lsn should have been preserved");

# The conflict detection slot should be created
$result = $new_sub->safe_psql('postgres',
	"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
);
is($result, qq(t), "conflict detection slot exists");

# Resume the initial sync and wait until all tables of subscription
# 'regress_sub5' are synchronized
$new_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 10");
$new_sub->restart;
$new_sub->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub5 ENABLE");
$new_sub->wait_for_subscription_sync($publisher, 'regress_sub5');

# wait for regress_sub4 to catchup as well
$publisher->wait_for_catchup('regress_sub4');

# Rows on tab_upgraded1 and tab_upgraded2 should have been replicated
$result =
  $new_sub->safe_psql('postgres', "SELECT count(*) FROM tab_upgraded1");
is($result, qq(51), "check replicated inserts on new subscriber");
$result =
  $new_sub->safe_psql('postgres', "SELECT count(*) FROM tab_upgraded2");
is($result, qq(1),
	"check the data is synced after enabling the subscription for the table that was in init state"
);

done_testing();
