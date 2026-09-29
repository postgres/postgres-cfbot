# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test continuous credential validation for TLS client certificate
# authentication: a session that authenticated with a client certificate
# must be terminated once that certificate passes its notAfter date, even
# though the certificate was valid at connection time.

use strict;
use warnings FATAL => 'all';
use Cwd qw(abs_path);
use POSIX qw(strftime);
use File::Copy qw(copy);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use SSL::Server;

if ($ENV{with_ssl} ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}
if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bssl\b/)
{
	plan skip_all =>
	  'Potentially unsafe test SSL not enabled in PG_TEST_EXTRA';
}

my $ssl_server = SSL::Server->new();

# This is the hostname used to connect to the server.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

# How long the runtime-generated client certificate stays valid, and how
# often the server re-validates credentials.  The certificate must outlive
# connection setup but expire well within the test's wait window.
my $cert_validity_secs = 15;
my $validation_interval = 5;	# minimum allowed by the GUC

# 1. Initialize and start the cluster with continuous validation enabled.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"credential_validation_enabled = on\n");
$node->append_conf('postgresql.conf',
	"credential_validation_interval = $validation_interval\n");
$node->start;

# 2. Configure the server for SSL.  This creates the "ssltestuser" role and
# the "certdb"/"verifydb" databases.  Regardless of the base auth method
# passed here, the generated HBA always serves "certdb" with plain "cert"
# (client-certificate-only) authentication; "verifydb" instead combines the
# base auth method with "clientcert=verify-full", which is what Test 2 below
# uses to exercise a certificate re-check alongside a non-cert primary auth
# method.  ssltestuser's password is set here so scram-sha-256 works on
# "verifydb".
my $password = 'ssltestpass';
$ssl_server->configure_test_server_for_ssl(
	$node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'scram-sha-256',
	password => $password,
	password_enc => 'scram-sha-256');
$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');

my $client_ca_crt = abs_path('ssl/client_ca.crt');
my $client_ca_key = abs_path('ssl/client_ca.key');
my $client_key = abs_path('ssl/client.key');

# Mint a short-lived client certificate for CN=ssltestuser, signed by the
# committed test client CA, into its own scratch directory.  We use
# "openssl ca -startdate/-enddate" (rather than "x509 -req -not_after")
# because those options work back to OpenSSL 1.1.1, which core still
# supports.  Each call gets a fresh tempdir/CA database, since two "openssl
# ca" invocations sharing one index/serial file would otherwise need careful
# sequencing.
sub mint_short_lived_client_cert
{
	my ($label) = @_;
	my $tempdir = PostgreSQL::Test::Utils::tempdir();

	# A throwaway "openssl ca" environment in the temp directory: the CA
	# cert/key are the committed ones, but the index, serial and new-cert
	# directory are scratch state we create here.
	mkdir "$tempdir/newcerts" or die "could not create newcerts dir: $!";
	PostgreSQL::Test::Utils::append_to_file("$tempdir/index.txt", '');
	PostgreSQL::Test::Utils::append_to_file("$tempdir/serial.txt", "1000\n");

	# OpenSSL's config parser treats backslashes as escape characters, so
	# paths embedded in the config file below must use forward slashes;
	# otherwise a Windows path such as "D:\a\..." is mangled (\a becomes a
	# bell character) and "openssl ca" cannot find its
	# database/new_certs_dir/certificate.  Forward slashes work fine for
	# file access on Windows too.
	(my $ca_crt_fwd = $client_ca_crt) =~ s{\\}{/}g;
	(my $ca_key_fwd = $client_ca_key) =~ s{\\}{/}g;
	(my $tempdir_fwd = $tempdir) =~ s{\\}{/}g;

	my $ca_config = <<EOF;
[ short_client_ca ]
certificate   = $ca_crt_fwd
private_key   = $ca_key_fwd
database      = $tempdir_fwd/index.txt
serial        = $tempdir_fwd/serial.txt
new_certs_dir = $tempdir_fwd/newcerts
default_md    = sha256
default_days  = 1
policy        = policy_match
email_in_dn   = no
unique_subject = no

[ policy_match ]
commonName = supplied
EOF
	PostgreSQL::Test::Utils::append_to_file("$tempdir/ca.config", $ca_config);

	# notBefore is set well in the past to avoid any clock-skew rejection at
	# connect time; notAfter is a few seconds out so the certificate expires
	# while the session is alive.
	my $now = time();
	my $startdate = strftime("%Y%m%d%H%M%SZ", gmtime($now - 300));
	my $enddate =
	  strftime("%Y%m%d%H%M%SZ", gmtime($now + $cert_validity_secs));

	PostgreSQL::Test::Utils::system_or_bail(
		'openssl', 'req', '-new',
		'-key' => $client_key,
		'-subj' => '/CN=ssltestuser',
		'-out' => "$tempdir/$label.csr");

	PostgreSQL::Test::Utils::system_or_bail(
		'openssl', 'ca', '-batch', '-notext',
		'-config' => "$tempdir/ca.config",
		'-name' => 'short_client_ca',
		'-startdate' => $startdate,
		'-enddate' => $enddate,
		'-in' => "$tempdir/$label.csr",
		'-out' => "$tempdir/$label.crt");

	# libpq refuses a group/world-readable private key, so use a 0600 copy.
	copy($client_key, "$tempdir/$label.key")
	  or die "could not copy client key: $!";
	chmod 0600, "$tempdir/$label.key"
	  or die "could not chmod client key: $!";

	return ("$tempdir/$label.crt", "$tempdir/$label.key");
}

#############################################################################
# Tests 1 and 2 both need "a session whose client certificate naturally
# expires mid-session, revalidated after one interval elapses".  They use
# independent certificates/sessions (though the same underlying
# "ssltestuser" role on the wire), so both certificates are minted and both
# sessions opened up front, and they share a single sleep() for the
# certificate expiry + validation interval to elapse, rather than each test
# waiting out its own separate window.  Because both use the exact same
# failure text ("certificate has expired" for "ssltestuser"), each assertion
# below is scoped to its own session's backend PID (via a log-line-prefix
# match), so Test 1's and Test 2's log lines can never satisfy each other's
# checks.
#############################################################################

#############################################################################
# Test 1: a session authenticated purely with a client certificate ("cert"
# HBA method) is terminated once that certificate passes its notAfter date.
#############################################################################
note "=== Test 1: pure client-certificate authentication ===";

my ($cert1, $key1) = mint_short_lived_client_cert('short1');

my $connstr1 =
	"host=$SERVERHOSTADDR port=" . $node->port . " dbname=certdb "
  . "user=ssltestuser sslmode=verify-ca "
  . "sslrootcert=ssl/root+server_ca.crt "
  . "sslcert=$cert1 sslkey=$key1";

my $session1 = $node->background_psql(
	'certdb',
	connstr => $connstr1,
	on_error_stop => 0);

# The certificate is still valid, so the session works normally.
my ($stdout, $ret) = $session1->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'cert session works while certificate is valid');
is($ret, 0, 'no error on initial query for cert session');

my $pid1;
($stdout, $ret) = $session1->query('SELECT pg_backend_pid();');
$pid1 = $1 if $stdout =~ /(\d+)/;
ok(defined $pid1, 'got session1 backend pid');

#############################################################################
# Test 2: a session authenticated with a *different* primary auth method
# (scram-sha-256) that also required a client certificate
# ("clientcert=verify-full") must still be terminated once that certificate
# expires.  Regression test for a gap where CheckCredentialValidity() only
# ran the validator for the primary auth method (CVT_COUNT for scram-sha-256,
# i.e. none), so a certificate required only as a second factor was never
# rechecked, even though the session's continued trust still depended on it.
#############################################################################
note "=== Test 2: scram-sha-256 + clientcert=verify-full also rechecks the certificate ===";

my ($cert2, $key2) = mint_short_lived_client_cert('short2');

$ENV{PGPASSWORD} = $password;
my $connstr2 =
	"host=$SERVERHOSTADDR port=" . $node->port . " dbname=verifydb "
  . "user=ssltestuser sslmode=verify-ca "
  . "sslrootcert=ssl/root+server_ca.crt "
  . "sslcert=$cert2 sslkey=$key2";

my $session2 = $node->background_psql(
	'verifydb',
	connstr => $connstr2,
	on_error_stop => 0);

($stdout, $ret) = $session2->query('SELECT 1 AS success;');
like($stdout, qr/1/,
	'2FA (scram + clientcert) session works while certificate is valid');
is($ret, 0, 'no error on initial query for 2FA session');

my $pid2;
($stdout, $ret) = $session2->query('SELECT pg_backend_pid();');
$pid2 = $1 if $stdout =~ /(\d+)/;
ok(defined $pid2, 'got session2 backend pid');

# Both certificates were minted moments apart with the same validity period,
# so a single wait covers both expiring plus one further validation cycle.
# Credential validation now also runs while a session sits idle waiting for
# the next message (not only once one is sent), so the FATAL for each can
# already be logged during this sleep, before either session is queried
# again below.
my $wait = $cert_validity_secs + $validation_interval + 2;
note "waiting $wait seconds for both client certificates to expire...";
sleep($wait);

#############################################################################
# Test 1 checks
#############################################################################
eval { $session1->query('SELECT 2 AS failure_expected;'); };

my $log_contents = slurp_file($node->logfile);
like(
	$log_contents,
	qr/backend\[$pid1\].*FATAL:.*session credentials have expired/,
	'cert session terminated after the client certificate expired');

# The DETAIL line (server log only, never sent to the client) should name
# the exact user and reason, so an admin doesn't have to guess which
# session/user this PID belonged to or which validator rejected it.
like(
	$log_contents,
	qr/backend\[$pid1\].*DETAIL:.*client certificate check failed for user "ssltestuser": certificate has expired/,
	'server log DETAIL identifies the user and the certificate-expiry reason');

eval { $session1->quit; };

#############################################################################
# Test 2 checks
#############################################################################
eval { $session2->query('SELECT 2 AS failure_expected;'); };

$log_contents = slurp_file($node->logfile);
like(
	$log_contents,
	qr/backend\[$pid2\].*FATAL:.*session credentials have expired/,
	'2FA session terminated after the client certificate expired, despite scram-sha-256 being the primary auth method'
);
like(
	$log_contents,
	qr/backend\[$pid2\].*DETAIL:.*client certificate check failed for user "ssltestuser": certificate has expired/,
	'server log DETAIL identifies the certificate-expiry reason, even though scram-sha-256 was the primary auth method'
);

eval { $session2->quit; };

#############################################################################
# Test 3: a session authenticated with a client certificate must be
# terminated once that certificate is revoked via CRL, even though it was
# never expired and even though nothing forces the already-forked backend
# serving this session to reload the CRL that was (or wasn't) in effect
# when it started -- only the postmaster and freshly-forked backends reload
# ssl_crl_file on SIGHUP.  This exercises be_tls_get_peer_cert_revoked()'s
# own direct, on-demand re-read of ssl_crl_file, not the handshake-time CRL
# check that already existed.
#############################################################################
note "=== Test 3: client certificate revoked via CRL mid-session ===";

my $crl_tempdir = PostgreSQL::Test::Utils::tempdir();
mkdir "$crl_tempdir/newcerts" or die "could not create newcerts dir: $!";
PostgreSQL::Test::Utils::append_to_file("$crl_tempdir/index.txt", '');
PostgreSQL::Test::Utils::append_to_file("$crl_tempdir/serial.txt", "2000\n");

(my $crl_ca_crt_fwd = $client_ca_crt) =~ s{\\}{/}g;
(my $crl_ca_key_fwd = $client_ca_key) =~ s{\\}{/}g;
(my $crl_tempdir_fwd = $crl_tempdir) =~ s{\\}{/}g;

my $crl_ca_config = <<EOF;
[ca]
default_ca = myca

[myca]
certificate      = $crl_ca_crt_fwd
private_key      = $crl_ca_key_fwd
database         = $crl_tempdir_fwd/index.txt
serial           = $crl_tempdir_fwd/serial.txt
new_certs_dir    = $crl_tempdir_fwd/newcerts
default_md       = sha256
default_crl_days = 3650
policy           = policy_anything

[policy_anything]
commonName = supplied
EOF
PostgreSQL::Test::Utils::append_to_file("$crl_tempdir/ca.config", $crl_ca_config);

# libpq refuses a group/world-readable private key, so use a 0600 copy (the
# committed ssl/client.key is not 0600 as checked out).
copy($client_key, "$crl_tempdir/client.key")
  or die "could not copy client key: $!";
chmod 0600, "$crl_tempdir/client.key"
  or die "could not chmod client key: $!";

# 3a. Open a session with the long-lived, committed client certificate
# (valid until 2050), so this test exercises revocation specifically, not
# expiry.
my $connstr3 =
	"host=$SERVERHOSTADDR port=" . $node->port . " dbname=certdb "
  . "user=ssltestuser sslmode=verify-ca "
  . "sslrootcert=ssl/root+server_ca.crt "
  . "sslcert=ssl/client.crt sslkey=$crl_tempdir/client.key";

my $session3 = $node->background_psql(
	'certdb',
	connstr => $connstr3,
	on_error_stop => 0);

($stdout, $ret) = $session3->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'cert session works before the certificate is revoked');
is($ret, 0, 'no error on initial query for the revocation test session');

# 3b. Revoke that certificate and publish a fresh CRL.
PostgreSQL::Test::Utils::system_or_bail(
	'openssl', 'ca',
	'-config' => "$crl_tempdir/ca.config",
	'-name' => 'myca',
	'-revoke' => 'ssl/client.crt',
	'-cert' => $client_ca_crt,
	'-keyfile' => $client_ca_key);

PostgreSQL::Test::Utils::system_or_bail(
	'openssl', 'ca',
	'-config' => "$crl_tempdir/ca.config",
	'-name' => 'myca',
	'-gencrl',
	'-cert' => $client_ca_crt,
	'-keyfile' => $client_ca_key,
	'-out' => "$crl_tempdir/root.crl");

$node->append_conf('postgresql.conf',
	"ssl_crl_file = '$crl_tempdir_fwd/root.crl'\n");
$node->reload;

# Only inspect log content generated from this point on, so this assertion
# can't be satisfied by Test 1's/Test 2's already-logged FATAL/DETAIL lines.
# Taken *before* the sleep: credential validation now also runs while the
# session sits idle waiting for the next message, so the FATAL can already
# be logged during the sleep below, before another command is ever sent.
my $test3_log_offset = -s $node->logfile;

# 3c. Wait for the next credential re-validation cycle to notice.
note
  "waiting $validation_interval seconds for credential validation to notice the revocation...";
sleep($validation_interval + 2);

eval { $session3->query('SELECT 2 AS failure_expected;'); };

$log_contents = slurp_file($node->logfile, $test3_log_offset);
like(
	$log_contents,
	qr/FATAL:.*session credentials have expired/,
	'cert session terminated after the client certificate was revoked via CRL'
);
like(
	$log_contents,
	qr/DETAIL:.*client certificate check failed for user "ssltestuser": certificate has been revoked/,
	'server log DETAIL distinguishes revocation from expiry');

eval { $session3->quit; };

$node->stop;
done_testing();
