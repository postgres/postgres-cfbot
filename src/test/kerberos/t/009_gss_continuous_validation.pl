
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test continuous credential validation for GSSAPI/Kerberos sessions.
#
# A session authenticated with GSSAPI keeps running independently of the
# Kerberos ticket that established it; nothing re-checks the ticket once the
# connection is up.  With credential_validation_enabled, the backend should
# periodically re-verify two things, and terminate the session when either
# fails:
#
# - the GSS security context (whose lifetime is bounded by the client's
#   ticket and the allowed clock skew) has not expired;
#
# - the KDC still accepts the session's principal.  This is checked without
#   sending any credential, and is only built with MIT Kerberos.
#
# This test sets up a KDC and opens four idle GSS sessions:
#
# - test1 has a short-lived ticket, so its context expires;
# - test2 is disabled at the KDC and test3 is deleted from it;
# - test4 is left alone.
#
# It checks that the first three are terminated, each with the matching
# reason in the server log, and that test4 is not.  It then stops the KDC to
# check that a KDC that does not answer within
# credential_validation_kdc_timeout leaves the session alone.
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

# The KDC principal check is only built where MIT Kerberos is available; the
# rest of the test does not depend on it.
my $have_kdc_check = check_pg_config('#define USE_KRB5_KDC_CHECK 1');

# Timing parameters.  The server-side GSS (acceptor) context lifetime is not
# the client's Kerberos ticket lifetime; it is bounded by the allowable clock
# skew.  We therefore configure a small clock skew and a short ticket for
# test1, so that its GSS context expires shortly after authentication.  The
# validation timer (minimum interval 5s) then notices within one more
# interval; the test waits for that to show up in the server log rather than
# sleeping a fixed time.
my $validation_interval = 5;
my $clockskew = 5;
my $ticket_lifetime = '4s';
my $kdc_timeout = 2;

my $host = 'auth-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $realm = 'EXAMPLE.COM';
my $password = 'secret1';

note "setting up Kerberos";

my $krb =
  PostgreSQL::Test::Kerberos->new($host, $hostaddr, $realm,
	clockskew => $clockskew);
$krb->create_principal($_, $password) for qw(test1 test2 test3 test4);

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
credential_validation_kdc_timeout = $kdc_timeout
});
$node->start;

$node->safe_psql('postgres', "CREATE USER $_;")
  for qw(test1 test2 test3 test4);

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
host all all $hostaddr/32 gss include_realm=0
});
$node->restart;

note "running tests";

# Open an idle GSS-authenticated session as $user, with a ticket of the given
# lifetime (or the default), and return it with its backend pid.
#
# Use gssencmode=disable so that GSSAPI is used only for authentication and the
# transport stays in plaintext.  With GSS transport encryption the expiring
# context would break the connection at the transport layer (and trigger a
# libpq reconnect) before the backend's credential validation runs; disabling
# it isolates the behavior under test -- the server terminating the session
# because its credentials expired.  Validation also runs while a session waits
# for its next command, so a session is terminated without being sent anything
# further.
sub open_session
{
	my ($user, %ticket) = @_;

	$krb->create_ticket($user, $password, %ticket);

	my $session = $node->background_psql(
		'postgres',
		connstr => $node->connstr('postgres')
		  . " user=$user host=$host hostaddr=$hostaddr gssencmode=disable",
		on_error_stop => 0);

	my ($stdout, $ret) = $session->query("SELECT 'session-live';");
	like($stdout, qr/session-live/, "$user: session is live");

	($stdout, $ret) = $session->query('SELECT pg_backend_pid();');
	my $pid = $1 if $stdout =~ /(\d+)/;
	ok(defined $pid, "$user: got the backend pid");

	return ($session, $pid);
}

my ($session1, $pid1) = open_session('test1', lifetime => $ticket_lifetime);
my ($session2, $pid2) = open_session('test2');
my ($session3, $pid3) = open_session('test3');
my ($session4, $pid4) = open_session('test4');

if ($have_kdc_check)
{
	# Disable one principal and delete another at the KDC.
	system_or_bail('kadmin.local', '-q', 'modprinc +disallow_all_tix test2');
	system_or_bail('kadmin.local', '-q', 'delprinc -force test3');
}

# Wait for the server to terminate the sessions, rather than sleeping a fixed
# time.  The patterns are scoped to each backend's pid.
note
  "waiting for validation to notice the expired and the removed credentials...";
ok( $node->wait_for_log(
		qr/backend\[$pid1\].*FATAL:.*session credentials have expired/),
	'session with an expired GSS context is terminated');

# The server log, but not the client, says which check failed and for whom.
ok( $node->log_contains(
		qr/backend\[$pid1\].*DETAIL:.*GSSAPI check failed for user "test1": security context has expired/
	),
	'server log DETAIL identifies the user and the expired GSS context');

# A command sent after termination must not return a result.
my ($stdout, $ret);
eval { ($stdout, $ret) = $session1->query("SELECT 'after-expiry';"); };
unlike($stdout // '',
	qr/after-expiry/, 'command after expiry does not return a result');
eval { $session1->quit; };

if ($have_kdc_check)
{
	ok( $node->wait_for_log(
			qr/backend\[$pid2\].*FATAL:.*session credentials have expired/),
		'session of the disabled principal is terminated');
	ok( $node->wait_for_log(
			qr/backend\[$pid3\].*FATAL:.*session credentials have expired/),
		'session of the deleted principal is terminated');

	ok( $node->log_contains(
			qr/backend\[$pid2\].*DETAIL:.*GSSAPI check failed for user "test2": the KDC no longer accepts the Kerberos principal \(Client's credentials have been revoked\)/
		),
		'log DETAIL names the user and says the principal is disabled');
	ok( $node->log_contains(
			qr/backend\[$pid3\].*DETAIL:.*GSSAPI check failed for user "test3": the KDC no longer accepts the Kerberos principal \(Client 'test3\@EXAMPLE.COM' not found in Kerberos database\)/
		),
		'log DETAIL names the user and says the principal no longer exists');
}

# The untouched principal is still enabled, so its session must have survived
# all of those checks.
($stdout, $ret) = $session4->query("SELECT 'still-here';");
like($stdout, qr/still-here/,
	'session of the untouched principal is still open');

if ($have_kdc_check)
{
	# A KDC that does not answer must not end the session, and must not hold
	# the backend for longer than credential_validation_kdc_timeout.  A
	# stopped KDC keeps its sockets but never replies.
	my $kdc_pid = slurp_file($PostgreSQL::Test::Kerberos::kdc_pidfile);
	chomp $kdc_pid;
	my $log_offset = -s $node->logfile;
	kill 'STOP', $kdc_pid;

	note "waiting for validation to give up on the stopped KDC...";
	ok( $node->wait_for_log(
			qr/backend\[$pid4\].*LOG:.*could not reach the Kerberos KDC for credential validation of principal "test4\@EXAMPLE.COM"/,
			$log_offset),
		'an unreachable KDC is logged');

	kill 'CONT', $kdc_pid;

	($stdout, $ret) = $session4->query("SELECT 'survived-kdc-outage';");
	like($stdout, qr/survived-kdc-outage/,
		'session stays open when the KDC does not answer');
}

eval { $session4->quit; };
eval { $session2->quit; };
eval { $session3->quit; };

# The sessions must have actually authenticated via GSS for this to be a
# meaningful test of GSS credential validation.
ok( $node->log_contains(
		qq{connection authenticated: identity="test1\@$realm" method=gss}),
	'session was authenticated with GSS');

done_testing();
