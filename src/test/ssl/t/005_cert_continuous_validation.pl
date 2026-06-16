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
# the "certdb" database; regardless of the base auth method passed here, the
# generated HBA always serves "certdb" with "cert" (client-certificate)
# authentication, which is what this test connects with.
$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust');
$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');

# 3. Mint a short-lived client certificate for CN=ssltestuser, signed by the
# committed test client CA.  We use "openssl ca -startdate/-enddate" (rather
# than "x509 -req -not_after") because those options work back to OpenSSL
# 1.1.1, which core still supports.
my $tempdir = PostgreSQL::Test::Utils::tempdir();
my $client_ca_crt = abs_path('ssl/client_ca.crt');
my $client_ca_key = abs_path('ssl/client_ca.key');
my $client_key = abs_path('ssl/client.key');

# A throwaway "openssl ca" environment in the temp directory: the CA cert/key
# are the committed ones, but the index, serial and new-cert directory are
# scratch state we create here.
mkdir "$tempdir/newcerts" or die "could not create newcerts dir: $!";
PostgreSQL::Test::Utils::append_to_file("$tempdir/index.txt", '');
PostgreSQL::Test::Utils::append_to_file("$tempdir/serial.txt", "1000\n");

# OpenSSL's config parser treats backslashes as escape characters, so paths
# embedded in the config file below must use forward slashes; otherwise a
# Windows path such as "D:\a\..." is mangled (\a becomes a bell character) and
# "openssl ca" cannot find its database/new_certs_dir/certificate.  Forward
# slashes work fine for file access on Windows too.
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
# connect time; notAfter is a few seconds out so the certificate expires while
# the session is alive.
my $now = time();
my $startdate = strftime("%Y%m%d%H%M%SZ", gmtime($now - 300));
my $enddate = strftime("%Y%m%d%H%M%SZ", gmtime($now + $cert_validity_secs));

PostgreSQL::Test::Utils::system_or_bail(
	'openssl', 'req', '-new',
	'-key' => $client_key,
	'-subj' => '/CN=ssltestuser',
	'-out' => "$tempdir/short.csr");

PostgreSQL::Test::Utils::system_or_bail(
	'openssl', 'ca', '-batch', '-notext',
	'-config' => "$tempdir/ca.config",
	'-name' => 'short_client_ca',
	'-startdate' => $startdate,
	'-enddate' => $enddate,
	'-in' => "$tempdir/short.csr",
	'-out' => "$tempdir/short.crt");

# libpq refuses a group/world-readable private key, so use a 0600 copy.
copy($client_key, "$tempdir/short.key")
  or die "could not copy client key: $!";
chmod 0600, "$tempdir/short.key"
  or die "could not chmod client key: $!";

# 4. Open a persistent session authenticated with the short-lived certificate.
my $connstr =
	"host=$SERVERHOSTADDR port=" . $node->port . " dbname=certdb "
  . "user=ssltestuser sslmode=verify-ca "
  . "sslrootcert=ssl/root+server_ca.crt "
  . "sslcert=$tempdir/short.crt sslkey=$tempdir/short.key";

my $session = $node->background_psql(
	'certdb',
	connstr => $connstr,
	on_error_stop => 0);

# The certificate is still valid, so the session works normally.
my ($stdout, $ret) = $session->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'cert session works while certificate is valid');
is($ret, 0, 'no error on initial query for cert session');

# 5. Wait until the certificate has expired and at least one further
# validation interval has elapsed, so the server detects the expiry.
my $wait = $cert_validity_secs + $validation_interval + 2;
note "waiting $wait seconds for the client certificate to expire...";
sleep($wait);

# 6. The next query should find the session terminated.
eval { $session->query('SELECT 2 AS failure_expected;'); };

my $log_contents = slurp_file($node->logfile);
like(
	$log_contents,
	qr/FATAL:.*session credentials have expired/,
	'cert session terminated after the client certificate expired');

eval { $session->quit; };

$node->stop;
done_testing();
