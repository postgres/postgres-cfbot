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
	'pg_upgrade', '--no-sync', '--pg-commit-ts',
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

done_testing();
