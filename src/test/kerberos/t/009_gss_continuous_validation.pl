
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test continuous credential validation for GSSAPI/Kerberos sessions.
#
# A session authenticated with GSSAPI keeps running independently of the
# Kerberos ticket that established it; nothing re-checks the ticket once the
# connection is up.  With credential_validation_enabled, the backend should
# periodically re-verify that the GSS security context (whose lifetime is
# derived from the client's ticket) has not expired, and terminate the session
# once it has.
#
# This test sets up a KDC, obtains a short-lived ticket, opens a GSS session,
# lets the ticket (and therefore the server-side GSS context) expire while the
# session sits idle, and then verifies that the next command is rejected with a
# FATAL "session credentials have expired".
#
# Like 001_auth.pl this requires a full MIT Kerberos installation; see the
# README for details.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Kerberos;
use Test::More;

if ($ENV{with_gssapi} ne 'yes')
{
	plan skip_all => 'GSSAPI/Kerberos not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bkerberos\b/)
{
	plan skip_all =>
	  'Potentially unsafe test GSSAPI/Kerberos not enabled in PG_TEST_EXTRA';
}

# Timing parameters.  The server-side GSS (acceptor) context lifetime is not
# the client's Kerberos ticket lifetime; it is bounded by the allowable clock
# skew.  We therefore configure a small clock skew so that the GSS context
# expires shortly after authentication, and then sit idle long enough that the
# context is certainly expired ($idle_seconds > $clockskew) and the validation
# timer has certainly fired ($idle_seconds > $validation_interval, whose
# minimum is 5s) before issuing the next command.
my $validation_interval = 5;
my $clockskew = 5;
my $ticket_lifetime = '15s';
my $idle_seconds = 25;

my $dbname = 'postgres';
my $username = 'test1';

note "setting up Kerberos";

my $host = 'auth-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $realm = 'EXAMPLE.COM';

my $krb =
  PostgreSQL::Test::Kerberos->new($host, $hostaddr, $realm,
	clockskew => $clockskew);

my $test1_password = 'secret1';
$krb->create_principal('test1', $test1_password);

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '$hostaddr'
krb_server_keyfile = '$krb->{keytab}'
log_connections = all
log_min_messages = debug2
lc_messages = 'C'
credential_validation_enabled = on
credential_validation_interval = $validation_interval
});
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
host all all $hostaddr/32 gss include_realm=0
});
$node->restart;

note "running tests";

# The server-side GSS (acceptor) context lifetime is bounded by the larger of
# the ticket's remaining lifetime and the allowable clock skew, so force both
# to be small: a short ticket together with the small clock skew configured
# above makes the GSS context expire shortly after the session is established.
$krb->create_ticket('test1', $test1_password, lifetime => $ticket_lifetime);

# Open a GSS-authenticated session, confirm it is live, then sit idle past the
# GSS context lifetime (and the validation interval) before issuing another
# command.
# The idle period is spent in a client-side "\! sleep", so the backend waits at
# a command boundary -- exactly where the validation timer's pending flag is
# acted upon when the next command arrives.
#
# Use gssencmode=disable so that GSSAPI is used only for authentication and the
# transport stays in plaintext.  With GSS transport encryption the expiring
# context would break the connection at the transport layer (and trigger a
# libpq reconnect) before the backend's credential validation runs; disabling
# it isolates the behavior under test -- the server terminating the session
# because its credentials expired.
my $connstr = $node->connstr('postgres')
  . " user=$username host=$host hostaddr=$hostaddr gssencmode=disable";

my $script =
  "SELECT 'session-live';\n" . "\\! sleep $idle_seconds\n" . "SELECT 'after-expiry';\n";

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', $script,
	connstr => $connstr,
	extra_params => ['-w']);

isnt($ret, 0, 'GSS session is terminated once its credentials expire');
like(
	$stdout,
	qr/session-live/,
	'session is live before the GSS credentials expire');
unlike(
	$stdout,
	qr/after-expiry/,
	'command after expiry does not return a result');
like(
	$stderr,
	qr/session credentials have expired/,
	'session terminated with the credential-expiry FATAL');

# The session must have actually authenticated via GSS for this to be a
# meaningful test of GSS credential validation.
ok( $node->log_contains(
		qq{connection authenticated: identity="test1\@$realm" method=gss}),
	'session was authenticated with GSS');

done_testing();
