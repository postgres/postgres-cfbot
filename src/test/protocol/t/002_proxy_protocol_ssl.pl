# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the PROXY protocol with sslnegotiation=direct.
# Run only if OpenSSL is enabled with ALPN support.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use File::Copy qw(copy);
use Test::More;

use ProxyProtocol;

if (($ENV{with_ssl} || '') ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}

unless (eval { require IO::Socket::SSL; 1 })
{
	plan skip_all => 'IO::Socket::SSL not available';
}
unless (IO::Socket::SSL->can('can_alpn') && IO::Socket::SSL->can_alpn)
{
	plan skip_all => 'IO::Socket::SSL lacks ALPN support';
}

my $host = '127.0.0.1';

# Network ranges trusted as proxies
my $loopback_net = "$host/32";
my $client_net = '192.0.2.0/24';    # TEST-NET-1, v1 TCP4 client

# Client (source) addresses carried in the PROXY headers.
my $v1_client = '192.0.2.1';        # in $client_net
my $v2_client = '192.0.2.5';        # in $client_net
my $unused_dest = '198.51.100.9';

# Source ports declared in the PROXY headers
my $v1_client_port = 56324;
my $v2_client_port = 40000;
my $unused_dest_port = 5432;

# Reuse the committed test certificate from the SSL test suite.
my $ssldir = "$FindBin::RealBin/../../ssl/ssl";
plan skip_all => "test certificate not found in $ssldir"
  unless -f "$ssldir/server-cn-only.crt";

my $node = PostgreSQL::Test::Cluster->new('proxy_protocol_ssl');
$node->init;

copy("$ssldir/server-cn-only.crt", $node->data_dir . '/server.crt')
  or die "could not copy server certificate: $!";
copy("$ssldir/server-cn-only.key", $node->data_dir . '/server.key')
  or die "could not copy server key: $!";
chmod 0600, $node->data_dir . '/server.key'
  or die "could not chmod server key: $!";

$node->append_conf('postgresql.conf',
		"listen_addresses = '$host'\n"
	  . "ssl = on\n"
	  . "ssl_cert_file = 'server.crt'\n"
	  . "ssl_key_file = 'server.key'\n"
	  . "proxy_networks = '$loopback_net'\n");
$node->append_conf('pg_hba.conf',
		"host all all $loopback_net trust\n"
	  . "host all all $client_net trust\n");

# cfbot's console output does not capture the server log
unless ($node->start(fail_ok => 1))
{
	diag("server log after failed start:\n"
		  . PostgreSQL::Test::Utils::slurp_file($node->logfile));
	BAIL_OUT('node failed to start');
}

my $port = $node->port;
my $user = $node->safe_psql('postgres', 'SELECT current_user');

set_connection(host => $host, port => $port, user => $user);

# A query to check the client address.
my $CLIENT_ADDR = 'SELECT host(inet_client_addr())';

# ----------------------------------------------------------------------------
# Protocol validation.
# ----------------------------------------------------------------------------
my $r = proxy_query_with_ssl(
	proxy_v1(4, $v1_client, $unused_dest, $v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v1 header before direct SSL: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v1_client,
	'v1 header before direct SSL: client address substituted');

$r = proxy_query_with_ssl(
	proxy_v2(4, $v2_client, $unused_dest, $v2_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v2 header before direct SSL: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v2_client,
	'v2 header before direct SSL: client address substituted');

# ----------------------------------------------------------------------------
# Logs validation.
# ----------------------------------------------------------------------------
my $logoff = -s $node->logfile;
$r = proxy_query_with_ssl(undef, $CLIENT_ADDR);
ok(!$r->{ok},
	'direct SSL with no PROXY header from a trusted peer is rejected');
$node->wait_for_log(
	qr/connection from a trusted proxy network must use the PROXY protocol/,
	$logoff);
ok(1, 'direct SSL without a header logs the PROXY requirement');

done_testing();
