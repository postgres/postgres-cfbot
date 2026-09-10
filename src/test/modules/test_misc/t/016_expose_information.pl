# Copyright (c) 2026, PostgreSQL Global Development Group

# Test gathering information before authentication via expose_* variables

# Force use of TCP/IP sockets via raw_connect
INIT{ $PostgreSQL::Test::Utils::use_unix_sockets = 0; }

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
plan tests => 31;

my $node = PostgreSQL::Test::Cluster->new('node1');

# Set as logical here so we can restart it as a replica later
$node->init(allows_streaming => 'logical');
$node->start;

my $server_version = $node->safe_psql('postgres', 'show server_version_num');
my $bindir = $node->config_data('--bindir');
my $datadir = $node->data_dir;
my $cdata = qx{$bindir/pg_controldata -D $datadir 2>&1};
my ($sysid) = $cdata =~ /Database system identifier:\s+(\d+)/;
my $receive_length = 200;

my ($socket, $response, $test);

sub call_socket {
	my $string = shift;
	$socket->close() if defined $socket;
	$socket = $node->raw_connect();
	$socket->timeout(1);
	$socket->send($string);
	$response = '';
	select(undef, undef, undef, 0.1);
	$socket->recv($response, $receive_length);
	return;
}

## Technically, this 'returns nothing' is a protocol error and a closed connection.
## But for purposes of this test, we just need to make sure nothing is returned.

$test = q{GET /foobar returns nothing when expose_information = ''};
call_socket('GET /foobar');
is ($response, '', $test);

$test = q{HEAD /foobar returns nothing when expose_information = ''};
call_socket('HEAD /foobar');
is ($response, '', $test);

$test = q{GET /replica returns nothing when expose_information=''};
call_socket('GET /replica');
is ($response, '', $test);

$test = q{HEAD /replica returns nothing when expose_information=''};
call_socket('HEAD /replica');
is ($response, '', $test);

$test = q{GET /primary returns nothing when expose_information=''};
call_socket('GET /primary');
is ($response, '', $test);

$test = q{HEAD /primary returns nothing when expose_information=''};
call_socket('HEAD /primary');
is ($response, '', $test);

$node->append_conf('postgresql.conf', "expose_information = 'role'");
$node->reload();

$test = q{GET /replica returns HTTP code 200 when expose_information = 'role' (primary)};
call_socket('GET /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /replica returns string "0" when expose_information = 'role' (primary)};
like ($response, qr{\r\n0\r\n}, $test);

$test = q{HEAD /replica returns HTTP code 503 when expose_information = 'role' (primary)};
call_socket('HEAD /replica');
like ($response, qr{^HTTP/1.1 503 }, $test);

$test = q{GET /primary returns HTTP code 200 when expose_information = 'role' (primary)};
call_socket('GET /primary');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /primary returns string "1" when expose_information = 'role' (primary)};
like ($response, qr{\r\n1\r\n}, $test);

$test = q{HEAD /primary returns HTTP code 200 when expose_information = 'role' (primary)};
call_socket('HEAD /primary');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /version returns nothing when expose_information = 'role'};
call_socket('GET /version');
is ($response, '', $test);

$test = q{GET /sysid returns nothing when expose_information = 'role'};
call_socket('GET /sysid');
is ($response, '', $test);

$node->append_conf('postgresql.conf', "expose_information= 'sysid'");
$node->reload();

$test = q{GET /replica returns nothing when expose_information = 'sysid'};
call_socket('GET /replica');
is ($response, '', $test);

$test = q{HEAD /replica returns nothing when expose_information = 'sysid'};
call_socket('HEAD /replica');
is ($response, '', $test);

$node->append_conf('postgresql.conf', "expose_information= 'sysid,role,version'");
$node->reload();

$test = q{GET /sysid returns correct value when expose_information contains 'sysid'};
call_socket('GET /sysid');
like ($response, qr/^$sysid\r\n/m, $test);

$test = q{GET /version returns correct value when expose_information contains 'version'};
call_socket('GET /version');
like ($response, qr/^$server_version\r\n/m, $test);

$test = q{GET /version\r\n returns correct value when expose_information contains 'version'};
call_socket("GET /version\r\n");
like ($response, qr/^$server_version\r\n/m, $test);

$test = q{GET /version HTTP/1.0 returns correct value when expose_information contains 'version'};
call_socket("GET /version HTTP/1.0");
like ($response, qr/^$server_version\r\n/m, $test);

$test = q{GET /ve returns nothing};
call_socket('GET /ve');
is ($response, '', $test);

$test = q{GET /versio returns nothing};
call_socket('GET /versio');
is ($response, '', $test);

$test = q{GET /versionx returns nothing};
call_socket('GET /versionx');
is ($response, '', $test);

$node->set_standby_mode();
$node->restart();

$test = q{GET /replica returns HTTP code 200 when expose_information contains 'role' (replica)};
call_socket('GET /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /replica returns string "1" when expose_information contains 'role' (replica)};
like ($response, qr{^1\r\n}m, $test);

$test = q{HEAD /replica returns HTTP code 200 when expose_information contains 'role' (replica)};
call_socket('HEAD /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /primary returns HTTP code 200 when expose_information contains 'role' (replica)};
call_socket('GET /primary');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /primary returns string "0" when expose_information contains 'role' (replica)};
like ($response, qr{^0\r\n}m, $test);

$test = q{HEAD /primary returns HTTP code 503 when expose_information contains 'role' (replica)};
call_socket('HEAD /primary');
like ($response, qr{^HTTP/1.1 503 }, $test);

$test = q{Regular connection still works after expose_information is enabled};
is ($node->safe_psql('postgres', 'select 42'), '42', $test);

$node->append_conf('postgresql.conf', "expose_information=''");
$node->reload();

$test = q{GET /version returns nothing after expose_information no longer has 'version'};
call_socket('GET /version');
is ($response, '', $test);

$socket->close();
