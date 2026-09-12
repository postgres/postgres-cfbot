
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

# Cluster setup which is shared for testing both load balancing methods
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

my $node_primary1 = PostgreSQL::Test::Cluster->new('primary1', port => $port);
$node_primary1->init(has_archiving => 1, allows_streaming => 1);

# Start it
$node_primary1->start;

# Take backup from which all operations will be run
$node_primary1->backup('my_backup');

my $node_standby = PostgreSQL::Test::Cluster->new('standby', port => $port, own_host => 1);
$node_standby->init_from_backup($node_primary1, 'my_backup',
	has_restoring => 1);
$node_standby->start();

my $node_primary2 = PostgreSQL::Test::Cluster->new('node1', port => $port, own_host => 1);
$node_primary2 ->init();
$node_primary2 ->start();

# target_session_attrs=primary should always choose the first one.
$node_primary1->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=primary try_all_addrs=1",
	"target_session_attrs=primary connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_primary1->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=read-write try_all_addrs=1",
	"target_session_attrs=read-write connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_primary1->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=any try_all_addrs=1",
	"target_session_attrs=any connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_standby->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=standby try_all_addrs=1",
	"target_session_attrs=standby connects to the third node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_standby->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=read-only try_all_addrs=1",
	"target_session_attrs=read-only connects to the third node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);


$node_primary1->stop();

# target_session_attrs=primary should always choose the first one.
$node_primary2->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=primary try_all_addrs=1",
	"target_session_attrs=primary connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_primary2->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=read-write try_all_addrs=1",
	"target_session_attrs=read-write connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_standby->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=any try_all_addrs=1",
	"target_session_attrs=any connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_standby->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=standby try_all_addrs=1",
	"target_session_attrs=standby connects to the third node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);
$node_standby->connect_ok(
	"host=pg-loadbalancetest port=$port target_session_attrs=read-only try_all_addrs=1",
	"target_session_attrs=read-only connects to the third node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);

$node_primary2->stop();
$node_standby->stop();


done_testing();
