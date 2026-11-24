
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

# credential_validation_interval has a 5s minimum.  The test waits for the
# termination to show up in the server log, not for a fixed time, so it takes
# at most one interval after the account is deleted.
my $validation_interval = 5;

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
credential_validation_ldap_timeout = 3
});
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all 127.0.0.1/32 ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapsearchfilter="(uid=\$username)"}
);
$node->restart;

note "running tests";

# Open an LDAP-authenticated session and confirm it is live, then delete the
# user's directory entry and leave the session idle.  Validation also runs
# while a session waits for its next command, so the session is terminated
# without being sent anything further: the re-search finds no entry.
my $connstr =
  $node->connstr('postgres') . " user=$username host=127.0.0.1 sslmode=disable";

local $ENV{PGPASSWORD} = 'secret1';

my $session = $node->background_psql(
	'postgres',
	connstr => $connstr,
	on_error_stop => 0);

my ($stdout, $ret) = $session->query("SELECT 'session-live';");
like(
	$stdout,
	qr/session-live/,
	'session is live before the LDAP account is removed');

my $pid;
($stdout, $ret) = $session->query('SELECT pg_backend_pid();');
$pid = $1 if $stdout =~ /(\d+)/;
ok(defined $pid, 'got the backend pid');

system_or_bail('ldapdelete', '-x', '-H', $ldap_url, '-D', $ldap_rootdn,
	'-y', $ldap_pwfile, $user_dn);

# Wait for the server to terminate the session, rather than sleeping a fixed
# time.  The patterns are scoped to this backend's pid.
note "waiting for validation to notice that the account is gone...";
ok( $node->wait_for_log(
		qr/backend\[$pid\].*FATAL:.*session credentials have expired/),
	'session terminated with the credential-expiry FATAL');

# The server log, but not the client, says which check failed and for whom.
ok( $node->log_contains(
		qr/backend\[$pid\].*DETAIL:.*LDAP check failed for user "$username": account no longer exists in the directory/
	),
	'server log DETAIL identifies the user and the missing LDAP account');

# A command sent after termination must not return a result.
eval { ($stdout, $ret) = $session->query("SELECT 'after-expiry';"); };
unlike(
	$stdout // '',
	qr/after-expiry/,
	'command after the account is removed does not return a result');
eval { $session->quit; };

# The session must have actually authenticated via LDAP for this to be a
# meaningful test of LDAP credential validation.
ok( $node->log_contains(
		qq{connection authenticated: identity="$user_dn" method=ldap}),
	'session was authenticated with LDAP');

done_testing();
