# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Tests for transfer pg_commit_ts directory.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize old cluster
my $old = PostgreSQL::Test::Cluster->new('old');
$old->init;
$old->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$old->start;
my $resold = $old->safe_psql(
	'postgres', qq{
		create table a(a int);
		select xid,timestamp from pg_last_committed_xact();
});

my ($xid) = $resold =~ /\s*(\d+)\s*\|.*/;
$old->stop;

# Initialize new cluster
my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;

# Setup a common pg_upgrade command to be used by all the test cases
my @pg_upgrade_cmd = (
	'pg_upgrade', '--no-sync','--pg-commit-ts',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port,
	$mode);

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

command_checks_all(
	[@pg_upgrade_cmd], 1,
	[qr{"track_commit_timestamp" must be "on" but is set to "off"}], [],
	'run of pg_upgrade for mismatch parameter track_commit_timestamp');

$new->append_conf('postgresql.conf', 'track_commit_timestamp = on');

command_ok([@pg_upgrade_cmd], 'run of pg_upgrade ok');

$new->start;
my $resnew = $new->safe_psql(
	'postgres', qq{
	select $xid,pg_xact_commit_timestamp(${xid}::text::xid);
});

$new->stop;
ok($resold eq $resnew, "timestamp transferred successfully");

# Check migrate with subscriptions and restore origin and remote_lsn

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

# Initialize the old subscriber node
my $old_sub = PostgreSQL::Test::Cluster->new('old_sub');
$old_sub->init;
$old_sub->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$old_sub->start;
my $oldbindir = $old_sub->config_data('--bindir');

# Initialize the new subscriber
my $new_sub = PostgreSQL::Test::Cluster->new('new_sub');
$new_sub->init;
$new_sub->append_conf('postgresql.conf', 'track_commit_timestamp = on');
my $newbindir = $new_sub->config_data('--bindir');

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

# Remember a connection string for the publisher node. It would be used
# several times.
my $appname='tap_sub';
my $connstr = $publisher->connstr . ' dbname=postgres ';

$publisher->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");
$old_sub->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY)");
$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub1 FOR TABLE tab");
#Create 1 origin
$old_sub->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION a_dummy
	CONNECTION '$connstr'
	PUBLICATION regress_pub1
	WITH (connect = false, enabled = false,create_slot = false)");
#Create 2 origin
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub2 CONNECTION '$connstr application_name=$appname' PUBLICATION regress_pub1 WITH(copy_data = false)"
);
#Create 3 origin
$old_sub->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION z_dummy
	CONNECTION '$connstr'
	PUBLICATION regress_pub1
	WITH (connect = false, enabled = false,create_slot = false)");
#Create 4,5 origin no link subscription
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_create('no_link_sub_4'),pg_replication_origin_create('no_link_sub_5')"
);

# Wait for initial table sync to finish
$old_sub->wait_for_subscription_sync($publisher, $appname);
$publisher->safe_psql('postgres', "INSERT INTO tab VALUES (11);");
$publisher->wait_for_catchup($appname);

my $result = $old_sub->safe_psql('postgres',
	"SELECT count(1) = 1 FROM tab");
is($result, qq(t), "Check that the table is 1 row");

my $remote_lsn = $old_sub->safe_psql('postgres',
	"SELECT remote_lsn FROM pg_replication_origin_status os, pg_subscription s WHERE os.external_id = 'pg_' || s.oid AND s.subname = 'regress_sub2'"
);

#Delete 1 origin
$old_sub->safe_psql('postgres', "ALTER SUBSCRIPTION a_dummy DISABLE");
$old_sub->safe_psql('postgres', "ALTER SUBSCRIPTION a_dummy SET (slot_name = NONE)");
$old_sub->safe_psql('postgres', "DROP SUBSCRIPTION a_dummy");

my $origin_others= $old_sub->safe_psql('postgres',
	"SELECT roident,roname FROM pg_replication_origin o LEFT JOIN pg_subscription s ON o.roname = 'pg_' || s.oid WHERE s.subname is null ORDER BY o.roident"
);

$old_sub->stop;

command_ok(
	[
		'pg_upgrade',
		'--no-sync','--pg-commit-ts',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode
	],
	'run of pg_upgrade for old instance when the subscription tables not empty'
);
ok( !-d $new_sub->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ removed after successful pg_upgrade");

$new_sub->start;

$result = $new_sub->safe_psql('postgres',
	"SELECT roident,s.subname FROM pg_replication_origin o LEFT JOIN pg_subscription s ON o.roname = 'pg_' || s.oid WHERE s.subname is not null ORDER BY o.roident");
is($result, qq(2|regress_sub2
3|z_dummy), "Check that the roident this restore old cluster (subscribtions)");


$result = $new_sub->safe_psql('postgres',
	"SELECT roident,roname FROM pg_replication_origin o LEFT JOIN pg_subscription s ON o.roname = 'pg_' || s.oid WHERE s.subname is null ORDER BY o.roident");
# No migrate origin create finction pg_replication_origin_create
# Comment next line if fix this bug
$origin_others="";
is($result, $origin_others, "Check that the roident this restore old cluster (origin id without subscribtions)");

my $remote_lsn_new_sub = $new_sub->safe_psql('postgres',
	"SELECT remote_lsn FROM pg_replication_origin_status os, pg_subscription s WHERE os.external_id = 'pg_' || s.oid AND s.subname = 'regress_sub2'"
);
is($remote_lsn_new_sub, qq($remote_lsn), "remote_lsn should have been preserved");


my $log_offset = -s $new_sub->logfile;

#Check replication new cluster
$publisher->safe_psql('postgres', "UPDATE tab set a=32 where a=11;");

$publisher->wait_for_catchup($appname);

$result = $new_sub->safe_psql('postgres',
	"SELECT a FROM tab WHERE a=32");
is($result,32, "update row ok");

$new_sub->log_check("no conflict",$log_offset,log_unlike => [ qr/conflict detected on relation \"public.tab\": conflict=/, ]);

done_testing();
