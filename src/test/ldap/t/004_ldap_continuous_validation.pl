
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test continuous credential validation for LDAP-authenticated sessions.
#
# Unlike OAuth/cert/GSS, an LDAP session retains no credential with an
# intrinsic expiry: the password presented at connection time is used for the
# bind and then discarded.  With credential_validation_enabled, the backend
# should instead periodically re-check that the authenticated account still
# exists in the directory (and still matches the configured search filter), and
# terminate the session once it is gone.
#
# This test (search+bind mode) opens an LDAP-authenticated session, deletes the
# user's entry from the directory while the session sits idle, and then
# verifies that the next command is rejected with a FATAL "session credentials
# have expired".
#
# Like 001_auth.pl this requires a full OpenLDAP installation; see the README
# for details.

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use LdapServer;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_ldap} ne 'yes')
{
	plan skip_all => 'LDAP not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bldap\b/)
{
	plan skip_all =>
	  'Potentially unsafe test LDAP not enabled in PG_TEST_EXTRA';
}
elsif (!$LdapServer::setup)
{
	plan skip_all => $LdapServer::setup_error;
}

# Timing parameters.  credential_validation_interval has a 5s minimum, so sit
# idle comfortably longer than that (and longer than the interval) to be sure
# the validation timer has fired before the next command is issued.
my $validation_interval = 5;
my $idle_seconds = 12;

my $username = 'test1';
my $user_dn = "uid=$username,dc=example,dc=net";

note "setting up LDAP server";

my $ldap_rootpw = 'secret';
my $ldap = LdapServer->new($ldap_rootpw, 'anonymous');    # use anonymous auth
$ldap->ldapadd_file('authdata.ldif');
$ldap->ldapsetpw($user_dn, 'secret1');

my ($ldap_server, $ldap_port, $ldap_basedn, $ldap_rootdn, $ldap_pwfile,
	$ldap_url)
  = $ldap->prop(qw(server port basedn rootdn pwfile url));

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '127.0.0.1'
log_connections = all
log_min_messages = debug2
lc_messages = 'C'
credential_validation_enabled = on
credential_validation_interval = $validation_interval
});
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all 127.0.0.1/32 ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapsearchfilter="(uid=\$username)"}
);
$node->restart;

note "running tests";

# Open an LDAP-authenticated session, confirm it is live, then delete the
# user's directory entry and sit idle past the validation interval before
# issuing another command.
#
# The deletion and the idle wait are done from a client-side "\!" shell step,
# so the backend waits at a command boundary -- exactly where the validation
# timer's pending flag is acted upon when the next command arrives.  By then
# the account is gone, so the re-search finds no entry and the session is
# terminated.
my $connstr =
  $node->connstr('postgres') . " user=$username host=127.0.0.1 sslmode=disable";

my $delete_and_wait =
  "ldapdelete -x -H $ldap_url -D '$ldap_rootdn' -y $ldap_pwfile '$user_dn' && sleep $idle_seconds";

my $script =
    "SELECT 'session-live';\n"
  . "\\! $delete_and_wait\n"
  . "SELECT 'after-expiry';\n";

local $ENV{PGPASSWORD} = 'secret1';

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', $script,
	connstr => $connstr,
	extra_params => ['-w']);

isnt($ret, 0, 'LDAP session is terminated once the account is gone');
like(
	$stdout,
	qr/session-live/,
	'session is live before the LDAP account is removed');
unlike(
	$stdout,
	qr/after-expiry/,
	'command after the account is removed does not return a result');
like(
	$stderr,
	qr/session credentials have expired/,
	'session terminated with the credential-expiry FATAL');

# The session must have actually authenticated via LDAP for this to be a
# meaningful test of LDAP credential validation.
ok( $node->log_contains(
		qq{connection authenticated: identity="$user_dn" method=ldap}),
	'session was authenticated with LDAP');

done_testing();
