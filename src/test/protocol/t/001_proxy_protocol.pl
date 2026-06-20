# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the PROXY protocol.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use ProxyProtocol;

my $host = '127.0.0.1';

# Network ranges trusted as proxies
my $loopback_net = "$host/32";
my $loopback_v6_net = '::1/128';
my $client_net = '192.0.2.0/24';       # TEST-NET-1, v1 TCP4 client
my $client_v4_net = '198.51.100.0/24'; # TEST-NET-2, v2 TCP4 client
my $client_v6_net = '2001:db8::/20';   # documentation range, TCP6 clients
my $ident_net = '203.0.113.0/24';      # TEST-NET-3, mapped to ident
my $untrusted_net = '10.0.0.0/8';      # a network the loopback peer is not in

# Client (source) addresses carried in the PROXY headers.
my $v1_client_v4 = '192.0.2.1';        # in $client_net
my $v2_client_v4 = '198.51.100.5';     # in $client_v4_net
my $v1_client_v6 = '2001:db8::1';
my $v2_client_v6 = '2001:db8::dead';
my $ident_client = '203.0.113.20';     # in $ident_net
my $unused_dest_v4 = '198.51.100.9';
my $unused_dest_v6 = '2001:db8::2';

# Source ports declared in the PROXY headers
my $v1_client_port = 56324;
my $v2_client_port = 40000;
my $unused_dest_port = 5432;

my $node = PostgreSQL::Test::Cluster->new('proxy_protocol');
$node->init;
$node->append_conf('postgresql.conf',
		"listen_addresses = '$host'\n"
	  . "proxy_networks = '$loopback_net'\n"
	  . "log_line_prefix = 'PXLOG h=%h H=%H r=%r R=%R '\n"
	  . "log_statement = 'all'\n");
$node->append_conf('pg_hba.conf',
		"host all all $loopback_net trust\n"
	  . "host all all $loopback_v6_net trust\n"
	  . "host all all $client_net trust\n"
	  . "host all all $client_v4_net trust\n"
	  . "host all all $client_v6_net trust\n"
	  . "host all all $ident_net ident\n");
$node->start;

my $port = $node->port;
my $user = $node->safe_psql('postgres', 'SELECT current_user');

set_connection(host => $host, port => $port, user => $user);

my $unix_connect = sub { $node->raw_connect };

# A query to check the client address.
my $CLIENT_ADDR = 'SELECT host(inet_client_addr())';


# ----------------------------------------------------------------------------
# Protocol validation.
# ----------------------------------------------------------------------------
# Trusted peers over TCP.
my $r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v1 TCP4 header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v1_client_v4, 'v1 TCP4 header: client address substituted');

$r = proxy_query(
	proxy_v1(
		6, $v1_client_v6, $unused_dest_v6,
		$v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v1 TCP6 header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v1_client_v6, 'v1 TCP6 header: client address substituted');

$r = proxy_query(
	proxy_v2(
		4, $v2_client_v4, $unused_dest_v4,
		$v2_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v2 TCP4 header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v2_client_v4, 'v2 TCP4 header: client address substituted');

$r = proxy_query(
	proxy_v2(
		6, $v2_client_v6, $unused_dest_v6,
		$v2_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok($r->{ok}, 'v2 TCP6 header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v2_client_v6, 'v2 TCP6 header: client address substituted');

# TLV vectors after the address block are accepted and ignored.
my $tlv = "\x04" . pack('n', 3) . "abc"        # PP2_TYPE_NOOP, 3 bytes
  . "\xee" . pack('n', 600) . ("\x00" x 600);  # opaque type, spans many reads
$r = proxy_query(
	proxy_v2(
		4, $v2_client_v4, $unused_dest_v4,
		$v2_client_port, $unused_dest_port, $tlv),
	$CLIENT_ADDR);
ok($r->{ok}, 'v2 header with trailing TLVs: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $v2_client_v4,
	'v2 header with trailing TLVs: TLVs ignored, client address substituted');

# v1 UNKNOWN keeps the real peer address.
$r = proxy_query(proxy_v1_unknown(), $CLIENT_ADDR);
ok($r->{ok}, 'v1 UNKNOWN header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $host, 'v1 UNKNOWN header: real peer address kept');

# v2 LOCAL keeps the real peer address.
$r = proxy_query(proxy_v2_local(), $CLIENT_ADDR);
ok($r->{ok}, 'v2 LOCAL header: connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $host, 'v2 LOCAL header: real peer address kept');

# Unix socket.
SKIP:
{
	skip "Unix-domain sockets not in use on this platform", 6
	  unless $PostgreSQL::Test::Utils::use_unix_sockets
	  && $node->raw_connect_works;

	$node->append_conf('postgresql.conf',
		"proxy_networks = 'unix, $loopback_net'\n");
	$node->reload;

	# v1 TCP address from an Unix peer must pass
	$r = proxy_query(
		proxy_v1(
			4, $v1_client_v4, $unused_dest_v4,
			$v1_client_port, $unused_dest_port),
		$CLIENT_ADDR,
		$unix_connect);
	ok($r->{ok}, 'unix: v1 header over a Unix socket succeeds')
	  or diag("error: $r->{error}");
	is($r->{value}, $v1_client_v4,
		'unix: v1 header substitutes the parsed TCP client over a Unix socket'
	);

	# v2 TCP address from an Unix peer must pass
	$r = proxy_query(
		proxy_v2(
			4, $v2_client_v4, $unused_dest_v4,
			$v2_client_port, $unused_dest_port),
		$CLIENT_ADDR,
		$unix_connect);
	ok($r->{ok}, 'unix: v2 header over a Unix socket succeeds')
	  or diag("error: $r->{error}");
	is($r->{value}, $v2_client_v4,
		'unix: v2 header substitutes the parsed TCP client over a Unix socket'
	);

	# A trusted peer must lead with a PROXY header
	my $unix_logoff = -s $node->logfile;
	$r = proxy_query(undef, $CLIENT_ADDR, $unix_connect);
	ok(!$r->{ok},
		'unix: header-less connection over a Unix socket is rejected');
	$node->wait_for_log(
		qr/connection from a trusted proxy network must use the PROXY protocol/,
		$unix_logoff);
	ok(1,
		'unix: header-less Unix-socket connection logs the PROXY requirement'
	);

	$node->append_conf('postgresql.conf',
		"proxy_networks = '$loopback_net'\n");
	$node->reload;
}

# A trusted peer must lead with a PROXY header
# For plain.
my $logoff = -s $node->logfile;
$r = proxy_query(undef, $CLIENT_ADDR);
ok(!$r->{ok}, 'plain startup packet from a trusted peer is rejected');
$node->wait_for_log(
	qr/connection from a trusted proxy network must use the PROXY protocol/,
	$logoff);
ok(1, 'plain connection from a trusted network logs the PROXY requirement');

# For SSL.
$logoff = -s $node->logfile;
$r = ssl_request();
ok(!$r->{ok},
	'SSL negotiation request from a trusted peer with no PROXY header is rejected'
);
$node->wait_for_log(
	qr/connection from a trusted proxy network must use the PROXY protocol/,
	$logoff);
ok(1,
	'SSL-first connection from a trusted network logs the PROXY requirement');

# Reject ident authentication
$logoff = -s $node->logfile;
$r = proxy_query(
	proxy_v1(
		4, $ident_client, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok(!$r->{ok}, 'ident authentication over a proxied connection is rejected');
$node->wait_for_log(
	qr/ident authentication is not supported over connections using the PROXY protocol/,
	$logoff);
ok(1, 'ident over the PROXY protocol logs that the method is unsupported');

# Reject malformed header from a trusted peer.
$logoff = -s $node->logfile;
$r = proxy_query("PROXY BOGUS arguments here\r\n", $CLIENT_ADDR);
ok(!$r->{ok}, 'malformed v1 header from trusted peer is rejected');
$node->wait_for_log(qr/incomplete startup packet/, $logoff);
ok(1, 'malformed header logs the generic "incomplete startup packet"');

# Reject connection from an untrusted peer.
$node->append_conf('postgresql.conf', "proxy_networks = '$untrusted_net'\n");
$node->reload;

$logoff = -s $node->logfile;
$r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok(!$r->{ok}, 'header from untrusted peer is rejected');
$node->wait_for_log(qr/incomplete startup packet/, $logoff);
ok(1, 'untrusted header is handled as an incomplete startup packet');

# Accept peers outside of the proxy networks without the PROXY header
$r = proxy_query(undef, $CLIENT_ADDR);
ok($r->{ok}, 'header-less connection from untrusted peer succeeds')
  or diag("error: $r->{error}");
is($r->{value}, $host,
	'real peer address reported for an ordinary connection');

# Ignore header when proxy networks is empty
$node->append_conf('postgresql.conf', "proxy_networks = ''\n");
$node->reload;

$logoff = -s $node->logfile;
$r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	$CLIENT_ADDR);
ok(!$r->{ok}, 'header ignored when proxy_networks is empty');
$node->wait_for_log(qr/incomplete startup packet/, $logoff);
ok(1, 'disabled feature handles header as an incomplete startup packet');

# ----------------------------------------------------------------------------
# GUC validation.
# ----------------------------------------------------------------------------
my ($ret, $stdout, $stderr);

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER SYSTEM SET proxy_networks = 'not-an-address'");
isnt($ret, 0, 'invalid network specification is rejected');
like(
	$stderr,
	qr/invalid value for parameter "proxy_networks"/,
	'invalid network: error mentions the parameter');
like(
	$stderr,
	qr/Invalid network specification/,
	'invalid network: error shows the reason');

($ret, $stdout, $stderr) =
  $node->psql('postgres', "ALTER SYSTEM SET proxy_networks = '10.0.0.0/99'");
isnt($ret, 0, 'invalid CIDR mask length is rejected');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER SYSTEM SET proxy_networks = '$loopback_net, $untrusted_net, $loopback_v6_net'"
);
is($ret, 0, 'a list of valid networks is accepted')
  or diag("stderr: $stderr");

# Ensure the special "unix" token is accepted.
($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER SYSTEM SET proxy_networks = 'unix, $loopback_net'");
is($ret, 0, 'the "unix" token is accepted in proxy_networks')
  or diag("stderr: $stderr");

$node->safe_psql('postgres', 'ALTER SYSTEM RESET proxy_networks');


# ----------------------------------------------------------------------------
# pg_stat_activity validation.
# ----------------------------------------------------------------------------
$node->append_conf('postgresql.conf', "proxy_networks = '$loopback_net'\n");
$node->reload;

# v1 PROXY returns proxy_addr and proxy_port for the proxy and client_addr
# and client_port are parsed from the header.
my $STAT_PROXY = q{SELECT format('%s|%s|%s|%s|%s',
	proxy_addr, (proxy_hostname IS NULL), (proxy_port IS NOT NULL),
	client_addr, client_port)
	FROM pg_stat_activity WHERE pid = pg_backend_pid()};

$r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	$STAT_PROXY);
ok($r->{ok}, 'pg_stat_activity over a proxied connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, "$host|t|t|$v1_client_v4|$v1_client_port",
	'pg_stat_activity: proxy_addr and port are the proxy, client_addr and port are the real peer'
);

# v2 LOCAL returns null for proxy_addr and proxy_port.
my $STAT_PLAIN = q{SELECT format('%s|%s|%s',
	(proxy_addr IS NULL), (proxy_port IS NULL), client_addr)
	FROM pg_stat_activity WHERE pid = pg_backend_pid()};

$r = proxy_query(proxy_v2_local(), $STAT_PLAIN);
ok($r->{ok}, 'pg_stat_activity over a LOCAL connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, "t|t|$host",
	'pg_stat_activity: proxy_addr and port are null for a LOCAL command');

# No PROXY header from untrusted network returns null for proxy_addr and
# proxy_port.
$node->append_conf('postgresql.conf', "proxy_networks = '$untrusted_net'\n");
$node->reload;

$r = proxy_query(undef, $STAT_PLAIN);
ok($r->{ok}, 'pg_stat_activity over an ordinary connection succeeds')
  or diag("error: $r->{error}");
is($r->{value}, "t|t|$host",
	'pg_stat_activity: proxy_addr and port are null for ordinary connection');

SKIP:
{
	skip "Unix-domain sockets not in use on this platform", 2
	  unless $PostgreSQL::Test::Utils::use_unix_sockets
	  && $node->raw_connect_works;

	$node->append_conf('postgresql.conf',
		"proxy_networks = 'unix, $loopback_net'\n");
	$node->reload;

	my $stat_unix_proxy = q{SELECT format('%s|%s|%s|%s',
		(proxy_addr IS NULL), proxy_port, client_addr, client_port)
		FROM pg_stat_activity WHERE pid = pg_backend_pid()};
	$r = proxy_query(
		proxy_v1(
			4, $v1_client_v4, $unused_dest_v4,
			$v1_client_port, $unused_dest_port),
		$stat_unix_proxy,
		$unix_connect);
	ok($r->{ok},
		'pg_stat_activity: connection over a proxied Unix socket succeeds')
	  or diag("error: $r->{error}");
	is($r->{value}, "t|-1|$v1_client_v4|$v1_client_port",
		'pg_stat_activity: proxy_addr is NULL and proxy_port is -1, like a Unix-socket client'
	);
}

# ----------------------------------------------------------------------------
# Logs validation.
# ----------------------------------------------------------------------------
$node->append_conf('postgresql.conf', "proxy_networks = '$loopback_net'\n");
$node->reload;

# Check client and proxy escapes for a proxied connection.
$logoff = -s $node->logfile;
$r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	"SELECT 'pxlog-proxied'");
ok($r->{ok}, 'logging: proxied connection succeeds')
  or diag("error: $r->{error}");
$node->wait_for_log(
	qr{PXLOG h=\Q$v1_client_v4\E H=\Q$host\E r=\Q$v1_client_v4\E\($v1_client_port\) R=\Q$host\E\(\d+\) LOG:\s+statement: SELECT 'pxlog-proxied'},
	$logoff);
ok(1, 'logging: %h/%r show the client, %H/%R show the proxy');

# Ensure proxy escapes are empty for an ordinary connection.
$node->append_conf('postgresql.conf', "proxy_networks = '$untrusted_net'\n");
$node->reload;

$logoff = -s $node->logfile;
$r = proxy_query(undef, "SELECT 'pxlog-ordinary'");
ok($r->{ok}, 'logging over an ordinary connection succeeds')
  or diag("error: $r->{error}");
$node->wait_for_log(
	qr{PXLOG h=\Q$host\E H= r=\Q$host\E\(\d+\) R= LOG:\s+statement: SELECT 'pxlog-ordinary'},
	$logoff);
ok(1, 'logging: %H/%R are empty without the PROXY protocol');

# Test csvlog and jsonlog destinations.
$node->append_conf(
	'postgresql.conf',
	"logging_collector = on\n"
	  . "log_destination = 'stderr, csvlog, jsonlog'\n"
	  . "log_rotation_age = 0\n"
	  # Re-trust loopback for the proxied case below.
	  . "proxy_networks = '$loopback_net'\n");
$node->restart;

# Wait for the collector to report the csv/json file names.
my $clf_path = $node->data_dir . '/current_logfiles';
PostgreSQL::Test::Utils::wait_for_file($clf_path, qr/^csvlog /m);
PostgreSQL::Test::Utils::wait_for_file($clf_path, qr/^jsonlog /m);
my $current_logfiles = slurp_file($clf_path);

$r = proxy_query(
	proxy_v1(
		4, $v1_client_v4, $unused_dest_v4,
		$v1_client_port, $unused_dest_port),
	"SELECT 'pxlog-loggers-proxied'");
ok($r->{ok}, 'loggers: proxied connection succeeds')
  or diag("error: $r->{error}");

my $csvline =
  wait_for_logger_line($node, $current_logfiles, 'csvlog',
	'pxlog-loggers-proxied');
like($csvline, qr/"\Q$host\E:\d+"/, 'csvlog: proxy_host:proxy_port detected');

my $jsonline =
  wait_for_logger_line($node, $current_logfiles, 'jsonlog',
	'pxlog-loggers-proxied');
like($jsonline, qr/"proxy_host":"\Q$host\E"/,
	'jsonlog: proxy_host key holds the proxy host');
like($jsonline, qr/"proxy_port":\d+/,
	'jsonlog: proxy_port key holds the proxy port');

# Ordinary connection returns null for proxy_host and proxy_port.
$node->append_conf('postgresql.conf', "proxy_networks = '$untrusted_net'\n");
$node->reload;

$r = proxy_query(undef, "SELECT 'pxlog-loggers-ordinary'");
ok($r->{ok}, 'loggers: ordinary connection succeeds')
  or diag("error: $r->{error}");

$csvline =
  wait_for_logger_line($node, $current_logfiles, 'csvlog',
	'pxlog-loggers-ordinary');
like($csvline, qr/,$/,
	'csvlog: proxy_connection column is empty without the PROXY protocol');

$jsonline =
  wait_for_logger_line($node, $current_logfiles, 'jsonlog',
	'pxlog-loggers-ordinary');
unlike($jsonline, qr/"proxy_host"/,
	'jsonlog: proxy_host key is omitted without the PROXY protocol');
unlike($jsonline, qr/"proxy_port"/,
	'jsonlog: proxy_port key is omitted without the PROXY protocol');

SKIP:
{
	skip "Unix-domain sockets not in use on this platform", 5
	  unless $PostgreSQL::Test::Utils::use_unix_sockets
	  && $node->raw_connect_works;

	$node->append_conf('postgresql.conf',
		"proxy_networks = 'unix, $loopback_net'\n");
	$node->reload;

	$r = proxy_query(
		proxy_v1(
			4, $v1_client_v4, $unused_dest_v4,
			$v1_client_port, $unused_dest_port),
		"SELECT 'pxlog-loggers-unix'",
		$unix_connect);
	ok($r->{ok}, 'loggers: proxied Unix-socket connection succeeds')
	  or diag("error: $r->{error}");

	my $sl =
	  wait_for_logger_line($node, $current_logfiles, 'stderr',
		'pxlog-loggers-unix');
	like(
		$sl,
		qr/h=\Q$v1_client_v4\E H=\[local\] r=\Q$v1_client_v4\E\($v1_client_port\) R=\[local\] /,
		'stderr: %H/%R returns [local] with no port for a Unix-socket proxy');

	$csvline =
	  wait_for_logger_line($node, $current_logfiles, 'csvlog',
		'pxlog-loggers-unix');
	like($csvline, qr/"\[local\]"$/,
		'csvlog: trailing proxy_connection returns [local] for a Unix-socket proxy'
	);

	$jsonline =
	  wait_for_logger_line($node, $current_logfiles, 'jsonlog',
		'pxlog-loggers-unix');
	like($jsonline, qr/"proxy_host":"\[local\]"/,
		'jsonlog: proxy_host returns [local] for a Unix-socket proxy');
	unlike($jsonline, qr/"proxy_port"/,
		'jsonlog: proxy_port key is omitted for Unix-socket proxy');
}

done_testing();
