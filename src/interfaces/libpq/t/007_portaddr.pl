# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# This tests the portaddr connection parameter, which separates the port that
# libpq actually connects to from the port that identifies the server.
#
# portaddr only applies to TCP connections, so the server has to listen on a
# TCP port here, which is why this test is not enabled by default.

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bportaddr\b/)
{
	plan skip_all =>
	  'Potentially unsafe test portaddr not enabled in PG_TEST_EXTRA';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$node->start;

# The port the server really listens on, plus a port that nothing listens on.
# Reaching the server while naming the latter is only possible via portaddr.
my $realport = $node->port;
my $unusedport = PostgreSQL::Test::Cluster::get_free_port();

# Sanity check: without portaddr, the unused port is unreachable.
$node->connect_fails(
	"host=127.0.0.1 port=$unusedport",
	"connection to an unused port fails without portaddr",
	expected_stderr =>
	  qr/connection to server at "127\.0\.0\.1", port $unusedport failed/);

# portaddr determines the port we connect to ...
$node->connect_ok(
	"host=127.0.0.1 port=$unusedport portaddr=$realport",
	"portaddr determines the port connected to",
	sql => "SELECT 'connected'",
	expected_stdout => qr/^connected$/);

# ... while port still identifies the connection, as reported by PQport.
$node->connect_ok(
	"host=127.0.0.1 port=$unusedport portaddr=$realport",
	"PQport reports port, not portaddr",
	sql => "\\echo :PORT",
	expected_stdout => qr/^$unusedport$/);

# An empty portaddr means "connect to port", the historical behavior.
$node->connect_ok(
	"host=127.0.0.1 port=$realport portaddr=",
	"empty portaddr falls back to port",
	sql => "SELECT 'connected'",
	expected_stdout => qr/^connected$/);

# A connection failure reports the port we actually tried to reach.
$node->connect_fails(
	"host=127.0.0.1 port=$realport portaddr=$unusedport",
	"connection failure reports the portaddr port",
	expected_stderr =>
	  qr/connection to server at "127\.0\.0\.1", port $unusedport failed/);

# Invalid values are rejected the same way port is.
$node->connect_fails(
	"host=127.0.0.1 port=$realport portaddr=65536",
	"portaddr must be a valid port number",
	expected_stderr => qr/invalid port number: "65536"/);

$node->connect_fails(
	"host=127.0.0.1 port=$realport portaddr=notanumber",
	"portaddr must be an integer",
	expected_stderr =>
	  qr/invalid integer value "notanumber" for connection option "portaddr"/);

# The portaddr list must match the host list, unless it has a single element.
$node->connect_fails(
	"host=127.0.0.1,127.0.0.1 port=$unusedport portaddr=$realport,$realport,$realport",
	"portaddr list must match the host list",
	expected_stderr => qr/could not match 3 portaddr values to 2 hosts/);

# A single portaddr applies to every host.  The first host here is a socket
# path that does not exist, so it fails and the second host is tried; that
# second host can only be reached if the lone portaddr was copied to it.
$node->connect_ok(
	"host=/nonexistent,127.0.0.1 port=$unusedport portaddr=$realport",
	"a single portaddr applies to all hosts",
	sql => "SELECT 'connected'",
	expected_stdout => qr/^connected$/);

# An empty item in the list uses the corresponding port value.  The first host
# is directed at the unused port and fails, so the second one is tried, and it
# can only succeed by falling back to its port.
$node->connect_ok(
	"host=127.0.0.1,127.0.0.1 port=$unusedport,$realport portaddr=$unusedport,",
	"an empty list item falls back to the corresponding port",
	sql => "SELECT 'connected'",
	expected_stdout => qr/^connected$/);

# PGPORTADDR behaves the same as the parameter.
{
	local $ENV{PGPORTADDR} = $realport;

	$node->connect_ok(
		"host=127.0.0.1 port=$unusedport",
		"PGPORTADDR environment variable is honored",
		sql => "SELECT 'connected'",
		expected_stdout => qr/^connected$/);
}

# portaddr is ignored for Unix-domain socket connections, which are named by
# port; naming an unused port there must not change anything.
if ($use_unix_sockets)
{
	$node->connect_ok(
		$node->connstr('postgres') . " portaddr=$unusedport",
		"portaddr is ignored for Unix-domain socket connections",
		sql => "SELECT 'connected'",
		expected_stdout => qr/^connected$/);
}

# The password file is searched using port, never portaddr.  This is the point
# of the parameter: an entry written for the server's own port keeps matching
# when the connection is made through an intermediary on another port.
$node->safe_psql('postgres',
	"CREATE ROLE portaddr_role LOGIN PASSWORD 'secret'");

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', "local all all trust");
$node->append_conf('pg_hba.conf',
	"host all portaddr_role 127.0.0.1/32 scram-sha-256");
$node->append_conf('pg_hba.conf', "host all all 127.0.0.1/32 trust");
$node->reload;

my $pgpassfile = "${PostgreSQL::Test::Utils::tmp_check}/pgpass_portaddr";
$ENV{PGPASSFILE} = $pgpassfile;

# An entry keyed to the port that identifies the server matches, even though
# the connection is actually made to a different port.
unlink($pgpassfile);
append_to_file($pgpassfile,
	"127.0.0.1:$unusedport:postgres:portaddr_role:secret\n");
chmod 0600, $pgpassfile or die;

$node->connect_ok(
	"host=127.0.0.1 port=$unusedport portaddr=$realport user=portaddr_role",
	"password file is searched using port",
	sql => "SELECT 'authenticated'",
	expected_stdout => qr/^authenticated$/);

# Conversely, an entry keyed to the port actually connected to does not match.
unlink($pgpassfile);
append_to_file($pgpassfile,
	"127.0.0.1:$realport:postgres:portaddr_role:secret\n");
chmod 0600, $pgpassfile or die;

$node->connect_fails(
	"host=127.0.0.1 port=$unusedport portaddr=$realport user=portaddr_role",
	"password file is not searched using portaddr",
	expected_stderr => qr/no password supplied/);

unlink($pgpassfile);
delete $ENV{PGPASSFILE};

done_testing();
