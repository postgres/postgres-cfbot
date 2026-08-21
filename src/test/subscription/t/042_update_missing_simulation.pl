# Copyright (c) 2025, PostgreSQL Global Development Group

# Test the conflict detection and resolution in logical replication
# Not intended to be committed because quite heavy
# Here to demonstrate reproducibility with pgbench
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use IPC::Run qw(start finish);
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

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
	"CREATE TABLE tbl(a int PRIMARY key, data_pub int);");

# Create similar table on subscriber with additional index to disable HOT updates
$node_subscriber->safe_psql(
	'postgres',
	"CREATE TABLE tbl(a int PRIMARY key, data_pub int, data_sub int default 0);
	 CREATE INDEX data_index ON tbl(data_pub);");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tbl");

# Create the subscription
my $appname = 'tap_sub';
$node_subscriber->safe_psql(
	'postgres',
	"CREATE SUBSCRIPTION tap_sub
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION tap_pub");

my $num_rows = 10;
my $num_updates = 10000;
my $num_clients = 10;
$node_publisher->safe_psql('postgres', "INSERT INTO tbl SELECT i, i * i FROM generate_series(1,$num_rows) i");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Prepare small pgbench scripts as files
my $sub_sql = $node_subscriber->basedir . '/sub_update.sql';
my $pub_sql = $node_publisher->basedir . '/pub_delete.sql';

open my $fh1, '>', $sub_sql or die $!;
print $fh1 "\\set num random(1,$num_rows)\nUPDATE tbl SET data_sub = data_sub + 1 WHERE a = :num;\n";
close $fh1;

open my $fh2, '>', $pub_sql or die $!;
print $fh2 "\\set num random(1,$num_rows)\nUPDATE tbl SET data_pub = data_pub + 1 WHERE a = :num;\n";
close $fh2;

my @sub_cmd = (
	'pgbench',
	'--no-vacuum', "--client=$num_clients", '--jobs=4', '--exit-on-abort', "--transactions=$num_updates",
	'-p', $node_subscriber->port, '-h', $node_subscriber->host, '-f', $sub_sql, 'postgres'
);

my @pub_cmd = (
	'pgbench',
	'--no-vacuum', "--client=$num_clients", '--jobs=4', '--exit-on-abort', "--transactions=$num_updates",
	'-p', $node_publisher->port, '-h', $node_publisher->host, '-f', $pub_sql, 'postgres'
);

$node_subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
# This should never happen
$node_subscriber->safe_psql('postgres',
		"SELECT injection_points_attach('apply_handle_update_internal_update_missing', 'error')");
my $log_offset = -s $node_subscriber->logfile;

# Start both concurrently
my ($sub_out, $sub_err, $pub_out, $pub_err) = ('', '', '', '');
my $sub_h = start \@sub_cmd, '>', \$sub_out, '2>', \$sub_err;
my $pub_h = start \@pub_cmd, '>', \$pub_out, '2>', \$pub_err;

# Wait for completion
finish $sub_h;
finish $pub_h;

like($sub_out, qr/actually processed/, 'subscriber pgbench completed');
like($pub_out, qr/actually processed/, 'publisher pgbench completed');

# Let subscription catch up, then check expectations
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

ok(!$node_subscriber->log_contains(
		qr/ERROR:  error triggered for injection point apply_handle_update_internal_update_missing/,
		$log_offset), 'invalid conflict detected');

done_testing();
