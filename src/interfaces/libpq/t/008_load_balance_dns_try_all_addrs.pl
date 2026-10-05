# Copyright (c) 2023-2025, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use Config;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bload_balance\b/)
{
	plan skip_all =>
	  'Potentially unsafe test load_balance not enabled in PG_TEST_EXTRA';
}

my $can_bind_to_127_0_0_2 =
  $Config{osname} eq 'linux' || $PostgreSQL::Test::Utils::windows_os;

# Checks for the requirements for testing load balancing method 2
if (!$can_bind_to_127_0_0_2)
{
	plan skip_all => 'load_balance test only supported on Linux and Windows';
}

my $hosts_path;
if ($windows_os)
{
	$hosts_path = 'c:\Windows\System32\Drivers\etc\hosts';
}
else
{
	$hosts_path = '/etc/hosts';
}

my $hosts_content = PostgreSQL::Test::Utils::slurp_file($hosts_path);

my $hosts_count = () =
  $hosts_content =~ /127\.0\.0\.[1-3] pg-loadbalancetest/g;
if ($hosts_count != 3)
{
	# Host file is not prepared for this test
	plan skip_all => "hosts file was not prepared for DNS load balance test";
}

$PostgreSQL::Test::Cluster::use_tcp = 1;
$PostgreSQL::Test::Cluster::test_pghost = '127.0.0.1';

my $port = PostgreSQL::Test::Cluster::get_free_port();
local $Test::Builder::Level = $Test::Builder::Level + 1;
my $node_primary1 = PostgreSQL::Test::Cluster->new("primary1", port => $port);
$node_primary1->init(has_archiving => 1, allows_streaming => 1);

# Start it
$node_primary1->start();

# Take backup from which all operations will be run
$node_primary1->backup("my_backup");

my $node_standby = PostgreSQL::Test::Cluster->new("standby", port => $port, own_host => 1);
$node_standby->init_from_backup($node_primary1, "my_backup",
	has_restoring => 1);
$node_standby->start();

my $node_primary2 = PostgreSQL::Test::Cluster->new("node1", port => $port, own_host => 1);
$node_primary2->init();
$node_primary2->start();
sub test_target_session_attr {
	my $target_session_attrs = shift;
	my $test_num = shift;
	my $primary1_expect_traffic = shift;
	my $standby_expect_traffic = shift;
	my $primary2_expect_traffic = shift;
	# Statistically the following loop with load_balance_hosts=random will almost
	# certainly connect at least once to each of the nodes. The chance of that not
	# happening is so small that it's negligible: (2/3)^50 = 1.56832855e-9
	foreach my $i (1 .. 50)
	{
		$node_primary1->connect_ok(
			"host=pg-loadbalancetest port=$port load_balance_hosts=random target_session_attrs=${target_session_attrs} try_all_addrs=1",
			"repeated connections with random load balancing",
			sql => "SELECT 'connect${test_num}'");
	}
	my $node_primary1_occurrences = () =
	  $node_primary1->log_content() =~ /statement: SELECT 'connect${test_num}'/g;
	my $node_standby_occurrences = () =
	  $node_standby->log_content() =~ /statement: SELECT 'connect${test_num}'/g;
	my $node_primary2_occurrences = () =
	  $node_primary2->log_content() =~ /statement: SELECT 'connect${test_num}'/g;

	my $total_occurrences =
	  $node_primary1_occurrences + $node_standby_occurrences + $node_primary2_occurrences;

	if ($primary1_expect_traffic == 1) {
		ok($node_primary1_occurrences > 0, "received at least one connection on node primary1");
	}else{
		ok($node_primary1_occurrences == 0, "received no connections on node primary1");
	}
	if ($standby_expect_traffic == 1) {
		ok($node_standby_occurrences > 0, "received at least one connection on node standby");
	}else{
		ok($node_standby_occurrences == 0, "received no connections on node standby");
	}

	if ($primary2_expect_traffic == 1) {
		ok($node_primary2_occurrences > 0, "received at least one connection on node primary2");
	}else{
		ok($node_primary2_occurrences == 0, "received no connections on primary2");
	}

	ok($total_occurrences == 50, "received 50 connections across all nodes");
}

test_target_session_attr('any',
	1, 1, 1, 1);
test_target_session_attr('read-only',
	2, 0, 1, 0);
test_target_session_attr('read-write',
	3, 1, 0, 1);
test_target_session_attr('primary',
	4, 1, 0, 1);
test_target_session_attr('standby',
	5, 0, 1, 0);


$node_primary1->stop();
$node_primary2->stop();
$node_standby->stop();

done_testing();
