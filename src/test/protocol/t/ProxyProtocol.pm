# Copyright (c) 2026, PostgreSQL Global Development Group

=pod

=head1 NAME

ProxyProtocol - helpers for driving PROXY protocol connections in TAP tests

=head1 SYNOPSIS

  use lib $FindBin::RealBin;
  use ProxyProtocol;

  # Build PROXY headers.
  my $v1 = proxy_v1(4, '192.0.2.1', '198.51.100.9', 56324, 5432);
  my $v2 = proxy_v2(4, '203.0.113.5', '198.51.100.9', 40000, 5432);

  # Authenticate (trust method) and run one query over a connected socket.
  my $addr = authenticate_and_query($sock, $user,
      'SELECT host(inet_client_addr())');

  # Or bind the per-test connection details once and drive whole queries.
  set_connection(host => $host, port => $port, user => $user);
  my $r = proxy_query($v1, 'SELECT host(inet_client_addr())');

=head1 DESCRIPTION

A minimal hand-rolled implementation of the parts of the v3 frontend/backend
protocol and the PROXY protocol that the regression tests need.  It is enough
to prepend a PROXY header, authenticate with the trust method, and run a single
query over a raw socket, whether plaintext or TLS.

=cut

package ProxyProtocol;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use Socket qw(inet_aton inet_pton AF_INET6);
use IO::Socket::INET;

our @EXPORT = qw(
  startup_packet
  read_message
  error_text
  proxy_v1
  proxy_v1_unknown
  proxy_v2
  proxy_v2_local
  authenticate_and_query
  set_connection
  proxy_query
  ssl_request
  proxy_query_with_ssl
  logger_file_name
  wait_for_logger_line
);

use constant PROXY_V2_SIG =>
  "\x0d\x0a\x0d\x0a\x00\x0d\x0a\x51\x55\x49\x54\x0a";

sub startup_packet
{
	my (%params) = @_;
	my $body = pack('N', 0x00030000);    # protocol version 3.0
	for my $key (sort keys %params)
	{
		$body .= $key . "\0" . $params{$key} . "\0";
	}
	$body .= "\0";                       # empty key name terminates the list
	return pack('N', length($body) + 4) . $body;
}

# Returns undef when the peer closed the connection before $n bytes arrived, so
# callers can treat a server-side disconnect as a distinct outcome.
sub read_exact
{
	my ($sock, $n) = @_;
	my $buf = '';
	while (length($buf) < $n)
	{
		my $chunk;
		my $r = sysread($sock, $chunk, $n - length($buf));
		return if !defined $r || $r == 0;
		$buf .= $chunk;
	}
	return $buf;
}

# Read one typed backend message.  Returns ($type, $payload), or an empty list
# once the connection has closed.
sub read_message
{
	my ($sock) = @_;
	my $type = read_exact($sock, 1);
	return () unless defined $type;
	my $lenbytes = read_exact($sock, 4);
	return () unless defined $lenbytes;
	my $len = unpack('N', $lenbytes);
	my $payload = '';
	if ($len > 4)
	{
		$payload = read_exact($sock, $len - 4);
		return () unless defined $payload;
	}
	return ($type, $payload);
}

# Pull the human-readable text (the 'M' field) out of an ErrorResponse or
# NoticeResponse body, which is the only part the tests assert on.
sub error_text
{
	my ($payload) = @_;
	for my $field (split /\0/, $payload)
	{
		return substr($field, 1) if substr($field, 0, 1) eq 'M';
	}
	return '';
}

# Build the PROXY v1 payload.
sub proxy_v1
{
	my ($family, $src, $dst, $sport, $dport) = @_;
	my $proto = $family == 4 ? 'TCP4' : 'TCP6';
	return "PROXY $proto $src $dst $sport $dport\r\n";
}

# Build the PROXY v1 payload for the unknown command.
sub proxy_v1_unknown
{
	return "PROXY UNKNOWN\r\n";
}

# Build the PROXY v2 payload.
sub proxy_v2
{
	my ($family, $src, $dst, $sport, $dport, $tlv) = @_;
	$tlv = '' unless defined $tlv;
	my $vercmd = "\x21";    # version 2, command PROXY
	my ($fambyte, $addr);
	if ($family == 4)
	{
		$fambyte = "\x11";    # TCP4
		$addr =
			inet_aton($src)
		  . inet_aton($dst)
		  . pack('n', $sport)
		  . pack('n', $dport);
	}
	else
	{
		$fambyte = "\x21";    # TCP6
		$addr =
			inet_pton(AF_INET6, $src)
		  . inet_pton(AF_INET6, $dst)
		  . pack('n', $sport)
		  . pack('n', $dport);
	}
	return
		PROXY_V2_SIG
	  . $vercmd
	  . $fambyte
	  . pack('n', length($addr) + length($tlv))
	  . $addr
	  . $tlv;
}

# Build the PROXY v2 payload for the LOCAL command.
sub proxy_v2_local
{
	return PROXY_V2_SIG . "\x20" . "\x00"
	  . pack('n', 0);    # command LOCAL, AF_UNSPEC
}

# Drive a full session over an already-connected stream, whether plaintext or
# TLS, and return the first column of the first row (undef for NULL or no row).
# Dies with a descriptive message on EOF, a server ErrorResponse, or a non-zero
# authentication request, so callers can map any failure to a single eval.
sub authenticate_and_query
{
	my ($stream, $user, $query) = @_;

	print $stream startup_packet(user => $user, database => 'postgres');

	while (1)
	{
		my ($type, $payload) = read_message($stream);
		defined $type or die "connection closed during startup\n";
		die error_text($payload) . "\n" if $type eq 'E';
		if ($type eq 'R')
		{
			my $code = unpack('N', $payload);
			die "authentication required (code $code)\n" if $code != 0;
		}
		last if $type eq 'Z';    # ReadyForQuery
	}

	print $stream 'Q' . pack('N', length($query) + 5) . $query . "\0";

	my $value;
	while (1)
	{
		my ($type, $payload) = read_message($stream);
		defined $type or die "connection closed during query\n";
		die error_text($payload) . "\n" if $type eq 'E';
		if ($type eq 'D')        # DataRow
		{
			my $ncols = unpack('n', substr($payload, 0, 2));
			if ($ncols >= 1)
			{
				my $collen = unpack('N', substr($payload, 2, 4));
				$value =
				  ($collen == 0xFFFFFFFF)
				  ? undef
				  : substr($payload, 6, $collen);
			}
		}
		last if $type eq 'Z';
	}

	return $value;
}

# The per-test connection details that the query drivers below reuse, bound once
# with set_connection() so the individual calls need only the header and query.
my ($conn_host, $conn_port, $conn_user);

sub set_connection
{
	my (%params) = @_;
	$conn_host = $params{host};
	$conn_port = $params{port};
	$conn_user = $params{user};
	return;
}

# Connect to the server, optionally sending $prefix (a PROXY header) ahead of
# the startup packet, then authenticate and run a one-column query.  The
# connection defaults to TCP loopback.  Pass $connect, a coderef returning a
# connected socket, to use another transport such as a Unix socket.
#
# Returns a hash-ref with ok => 1 and the query value on success, or ok => 0
# and an error string otherwise.
sub proxy_query
{
	my ($prefix, $query, $connect) = @_;
	my $result = { ok => 0, error => 'unknown' };

	eval {
		local $SIG{ALRM} = sub { die "timeout\n" };
		alarm($PostgreSQL::Test::Utils::timeout_default);

		my $sock =
			$connect
		  ? $connect->()
		  : IO::Socket::INET->new(
			PeerHost => $conn_host,
			PeerPort => $conn_port,
			Proto => 'tcp');
		die "cannot connect: $!\n" unless $sock;
		$sock->autoflush(1);

		print $sock $prefix if defined $prefix;
		my $value = authenticate_and_query($sock, $conn_user, $query);

		close $sock;
		alarm(0);
		$result = { ok => 1, value => $value };
	};
	return { ok => 0, error => $@ } if $@;
	return $result;
}

# Open a raw connection and send an SSL negotiation request as the very first
# bytes, with no preceding PROXY header.
#
# Returns a hash-ref with ok => 1 if the request succeeded, or ok => 0 and an
# error string if it failed.
sub ssl_request
{
	my $result = { ok => 0, error => 'unknown' };

	eval {
		local $SIG{ALRM} = sub { die "timeout\n" };
		alarm($PostgreSQL::Test::Utils::timeout_default);

		my $sock = IO::Socket::INET->new(
			PeerHost => $conn_host,
			PeerPort => $conn_port,
			Proto => 'tcp') or die "cannot connect: $!\n";
		$sock->autoflush(1);

		# SSLRequest is a length of 8 followed by the negotiate-SSL request code.
		print $sock pack('N', 8) . pack('N', 80877103);

		while (1)
		{
			my ($type, $payload) = read_message($sock);
			die "connection closed\n" unless defined $type;
			die error_text($payload) . "\n" if $type eq 'E';
			last if $type eq 'Z';
		}
		close $sock;
		alarm(0);
		$result = { ok => 1 };
	};
	return { ok => 0, error => $@ } if $@;
	return $result;
}

# Connect over TCP, send $prefix (a PROXY header) in cleartext, then open a
# direct SSL connection (no SSLRequest) offering the PostgreSQL ALPN protocol,
# authenticate over TLS, and run a one-column query.  The caller must have
# loaded IO::Socket::SSL, which the module leaves optional.
#
# Returns a hash-ref with ok => 1 and the query value on success, or ok => 0
# and an error string otherwise.
sub proxy_query_with_ssl
{
	my ($prefix, $query) = @_;
	my $result = { ok => 0, error => 'unknown' };

	eval {
		local $SIG{ALRM} = sub { die "timeout\n" };
		alarm($PostgreSQL::Test::Utils::timeout_default);

		my $sock = IO::Socket::INET->new(
			PeerHost => $conn_host,
			PeerPort => $conn_port,
			Proto => 'tcp') or die "cannot connect: $!\n";
		$sock->autoflush(1);

		# The PROXY header travels in cleartext, ahead of the TLS handshake.
		print $sock $prefix if defined $prefix;

		my $ssl = IO::Socket::SSL->start_SSL(
			$sock,
			SSL_verify_mode => 0,
			SSL_alpn_protocols => ['postgresql'])
		  or die "TLS handshake failed: "
		  . (IO::Socket::SSL->errstr // 'unknown') . "\n";

		die "ALPN did not negotiate 'postgresql'\n"
		  unless defined $ssl->alpn_selected
		  && $ssl->alpn_selected eq 'postgresql';

		my $value = authenticate_and_query($ssl, $conn_user, $query);

		close $ssl;
		alarm(0);
		$result = { ok => 1, value => $value };
	};
	return { ok => 0, error => $@ } if $@;
	return $result;
}

# Given the contents of current_logfiles, return the file name (relative to the
# data directory) recorded for the given destination, 'csvlog' or 'jsonlog'.
sub logger_file_name
{
	my ($current_logfiles, $format) = @_;
	return ($current_logfiles =~ /^$format (.*)$/m) ? $1 : undef;
}

# Wait for the collected $format log file to contain $marker, then return the
# line carrying it.  A unique marker query is what pins a log record to one
# specific connection.
sub wait_for_logger_line
{
	my ($node, $current_logfiles, $format, $marker) = @_;
	my $path =
	  $node->data_dir . '/' . logger_file_name($current_logfiles, $format);

	PostgreSQL::Test::Utils::wait_for_file($path, quotemeta($marker));

	foreach my $line (split(/\n/, PostgreSQL::Test::Utils::slurp_file($path)))
	{
		return $line if index($line, $marker) >= 0;
	}
	return '';
}

1;
