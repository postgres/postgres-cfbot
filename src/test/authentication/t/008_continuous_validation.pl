use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$use_unix_sockets)
{
    plan skip_all => "authentication tests cannot run without Unix-domain sockets";
}

# Helper to reset pg_hba.conf with specific auth method for test users
sub reset_pg_hba
{
    my ($node, $hba_method, @users) = @_;

    unlink($node->data_dir . '/pg_hba.conf');
    # Each specified user uses the given method
    foreach my $user (@users)
    {
        $node->append_conf('pg_hba.conf', "local all $user $hba_method\n");
    }
    # Others use trust
    $node->append_conf('pg_hba.conf', "local all all trust\n");
    $node->reload;
}

# 1. Initialize and start the PostgreSQL cluster
my $node = PostgreSQL::Test::Cluster->new('main');
# allows_streaming => 'logical' sets wal_level and max_wal_senders high
# enough to accept a replication=database connection, needed by Test 11.
$node->init(allows_streaming => 'logical');

# Enable credential validation with short interval (5 seconds minimum)
$node->append_conf('postgresql.conf', "credential_validation_enabled = on\n");
$node->append_conf('postgresql.conf', "credential_validation_interval = 5\n");

# allows_streaming's max_connections = 10 (meant for running several
# concurrent test postmasters) is too low for this file's many simultaneous
# background_psql sessions; raise it back up.
$node->append_conf('postgresql.conf', "max_connections = 30\n");

$node->start;

#############################################################################
# Tests 1-5, 7a-7b and 10 all just need "a session, with some credential- or
# role-affecting change applied to it, revalidated after one interval
# elapses".  They use independent users/roles, so all of their setup and
# mutations are done up front here and they share a single sleep() for that
# interval to elapse, rather than each test waiting out its own separate
# interval.
#
# Test 6 (credential_validation_enabled turned off) and Test 8 (repeated
# ERRORs) each get their own isolated timing window below instead: Test 6
# flips a server-wide GUC that would suppress validation for every other
# session in this batch, and Test 8 needs its own sub-interval-granularity
# sleep loop.
#############################################################################
note "=== Setting up Tests 1-5, 7a-7b, 10 (batched validation window) ===";

$node->safe_psql('postgres', "CREATE USER user1 LOGIN PASSWORD 'secret1';");
$node->safe_psql('postgres', "CREATE USER user2 LOGIN PASSWORD 'secret2';");
$node->safe_psql('postgres', "CREATE USER user3 LOGIN PASSWORD 'secret3';");
$node->safe_psql('postgres', "CREATE USER user4 LOGIN PASSWORD 'secret4';");
$node->safe_psql('postgres', "CREATE USER user5 LOGIN;");
$node->safe_psql('postgres', "CREATE USER user10 LOGIN PASSWORD 'secret10';");
$node->safe_psql('postgres', "CREATE USER authsuper1 LOGIN SUPERUSER PASSWORD 'secret7a';");
$node->safe_psql('postgres', "CREATE ROLE sesstarget1;");
$node->safe_psql('postgres', "CREATE USER authsuper2 LOGIN SUPERUSER PASSWORD 'secret7b';");
$node->safe_psql('postgres', "CREATE ROLE sesstarget2;");

# user5 (Test 5) relies on the "all trust" fallback reset_pg_hba always
# appends, since trust-authenticated sessions have no authn_id and are
# skipped by credential validation entirely -- that's the very thing Test 5
# is checking.
reset_pg_hba($node, 'md5', 'user1', 'user2', 'user3', 'user4', 'user10',
    'authsuper1', 'authsuper2');

#############################################################################
# Test 1 setup: VALID UNTIL expiration
#############################################################################
$ENV{PGPASSWORD} = 'secret1';
my $session1 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);

my ($stdout, $ret) = $session1->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'user1 can execute queries initially');
is($ret, 0, 'no errors during initial query for user1');

#############################################################################
# Test 2 setup: user dropped while session is active
#############################################################################
$ENV{PGPASSWORD} = 'secret2';
my $session2 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user2']
);

($stdout, $ret) = $session2->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'user2 can execute queries initially');
is($ret, 0, 'no errors during initial query for user2');

#############################################################################
# Test 3 setup: VALID UNTIL extended keeps session alive (positive test)
#############################################################################
$ENV{PGPASSWORD} = 'secret3';
my $session3 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user3']
);

#############################################################################
# Test 4 setup: multiple sessions terminated when the same user expires
#############################################################################
$ENV{PGPASSWORD} = 'secret4';
my $session4a = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user4']
);
my $session4b = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user4']
);

($stdout, $ret) = $session4a->query('SELECT 1;');
like($stdout, qr/1/, 'session4a works initially');
($stdout, $ret) = $session4b->query('SELECT 1;');
like($stdout, qr/1/, 'session4b works initially');

#############################################################################
# Test 5 setup: trust auth sessions are not affected
#############################################################################
delete $ENV{PGPASSWORD};
my $session5 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user5']
);

#############################################################################
# Test 7a/7b setup: SET SESSION AUTHORIZATION does not change whose
# credentials are checked.  Regression test for using
# GetAuthenticatedUserId() (the role that actually authenticated) rather
# than GetSessionUserId() (the mutable current session role) in the
# baseline role-validity check.
#
# authsuperN logs in over the wire, so its credentials are what actually got
# authenticated.  sesstargetN is only ever reached via SET SESSION
# AUTHORIZATION, never over the wire, so it needs no pg_hba entry or
# password of its own.
#############################################################################
$ENV{PGPASSWORD} = 'secret7a';
my $session7a = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'authsuper1']
);

($stdout, $ret) = $session7a->query('SET SESSION AUTHORIZATION sesstarget1;');
is($ret, 0, 'Test 7a: SET SESSION AUTHORIZATION succeeds for superuser');
($stdout, $ret) = $session7a->query('SELECT current_user;');
like($stdout, qr/sesstarget1/, 'Test 7a: session is now running as sesstarget1');

$ENV{PGPASSWORD} = 'secret7b';
my $session7b = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'authsuper2']
);

($stdout, $ret) = $session7b->query('SET SESSION AUTHORIZATION sesstarget2;');
is($ret, 0, 'Test 7b: SET SESSION AUTHORIZATION succeeds for superuser');
($stdout, $ret) = $session7b->query('SELECT current_user;');
like($stdout, qr/sesstarget2/, 'Test 7b: session is now running as sesstarget2');

#############################################################################
# Test 10 setup: idle session terminated in real time, without ever sending
# another command.  Regression test for the idle-session enforcement gap:
# credential validation used to only run right after ReadCommand() returned
# a message (i.e. at the next command boundary), so a session that received
# no further commands after its credentials expired would sit in
# pg_stat_activity indefinitely, unlike idle_session_timeout which fires in
# real time even while blocked waiting for the next client message.
# ProcessClientReadInterrupt() now also runs a validation cycle from that
# same idle-wait point, so $session10 is never sent another query for the
# rest of this test -- only pg_stat_activity and the server log are
# observed.
#############################################################################
$ENV{PGPASSWORD} = 'secret10';
my $session10 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user10']
);

($stdout, $ret) = $session10->query('SELECT pg_backend_pid();');
my $pid10;
$pid10 = $1 if $stdout =~ /(\d+)/;
ok(defined $pid10, 'Test 10: got user10 backend pid');

#############################################################################
# Apply every mutation for this batch, then wait out a single validation
# interval that covers all of them.
#############################################################################
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL '2025-11-02 16:59:37+05:30';");
$node->safe_psql('postgres', "DROP USER user2;");
$node->safe_psql('postgres', "ALTER USER user3 VALID UNTIL '2099-12-31 23:59:59';");
$node->safe_psql('postgres', "ALTER USER user4 VALID UNTIL '2020-01-01';");
$node->safe_psql('postgres', "ALTER USER user5 VALID UNTIL '2020-01-01';"); # no-op: trust has no authn_id
$node->safe_psql('postgres', "ALTER USER authsuper1 VALID UNTIL '2020-01-01';");
$node->safe_psql('postgres', "ALTER ROLE sesstarget2 VALID UNTIL '2020-01-01';"); # session role only, irrelevant
$node->safe_psql('postgres', "ALTER USER user10 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for one shared credential validation cycle to cover Tests 1-5, 7a-7b, and 10...";
sleep(7);

#############################################################################
# Test 1 checks
#############################################################################
eval {
    ($stdout, $ret) = $session1->query('SELECT 2 AS failure_expected;');
};

my $log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 1: server log shows session terminated due to expired credentials'
);
like(
    $log_contents,
    qr/DETAIL:.*role validity check failed for user "user1": role has passed its VALID UNTIL expiration/,
    'Test 1: server log DETAIL identifies the user and the VALID UNTIL reason'
);

eval { $session1->quit; };

#############################################################################
# Test 2 checks
#############################################################################
eval {
    ($stdout, $ret) = $session2->query('SELECT 2 AS failure_expected;');
};

$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 2: server log shows session terminated after user was dropped'
);
like(
    $log_contents,
    qr/DETAIL:.*role validity check failed for user "user2": role no longer exists/,
    'Test 2: server log DETAIL identifies the user and the dropped-role reason'
);

eval { $session2->quit; };

#############################################################################
# Test 3 checks (positive)
#############################################################################
($stdout, $ret) = $session3->query('SELECT 1 AS still_alive;');
like($stdout, qr/1/, 'Test 3: session remains alive with valid VALID UNTIL');
is($ret, 0, 'Test 3: no errors when VALID UNTIL is in the future');

eval { $session3->quit; };

#############################################################################
# Test 4 checks
#############################################################################
eval { $session4a->query('SELECT 2;'); };
eval { $session4b->query('SELECT 2;'); };

$log_contents = slurp_file($node->logfile);
# Count occurrences of the termination message
my @matches = ($log_contents =~ /FATAL:.*session credentials have expired/g);
cmp_ok(scalar(@matches), '>=', 3, 'Test 4: multiple sessions terminated for same user');

eval { $session4a->quit; };
eval { $session4b->quit; };

#############################################################################
# Test 5 checks
#############################################################################
($stdout, $ret) = $session5->query('SELECT 1 AS trust_still_works;');
like($stdout, qr/1/, 'Test 5: trust auth session not terminated (no validator)');

eval { $session5->quit; };

#############################################################################
# Test 7a checks: the *originally authenticated* role's expiry still
# terminates the session even after switching to a still-valid session
# role.  If the baseline check regressed to GetSessionUserId(), this session
# would survive instead (sesstarget1 is never touched), so this test would
# catch that.
#############################################################################
eval {
    ($stdout, $ret) = $session7a->query('SELECT 2 AS failure_expected;');
};

$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 7a: session terminated on original login role expiry, despite SET SESSION AUTHORIZATION to a still-valid role'
);
like(
    $log_contents,
    qr/DETAIL:.*role validity check failed for user "authsuper1": role has passed its VALID UNTIL expiration/,
    'Test 7a: server log DETAIL identifies the originally authenticated role, not the session role'
);

eval { $session7a->quit; };

#############################################################################
# Test 7b checks: the *current session role's* own expiry is irrelevant --
# only the originally authenticated role is checked.  If the baseline check
# regressed to GetSessionUserId(), this session would be wrongly terminated
# instead of surviving, so this test would catch that.
#############################################################################
($stdout, $ret) = $session7b->query('SELECT 1 AS still_alive;');
like($stdout, qr/1/, 'Test 7b: session survives when only the session role (not the login role) has expired');
is($ret, 0, 'Test 7b: no errors -- validation checks the authenticated role, not the session role');

eval { $session7b->quit; };

#############################################################################
# Test 10 checks
#############################################################################
my $still_present = $node->safe_psql('postgres',
    "SELECT count(*) FROM pg_stat_activity WHERE pid = $pid10;");
is($still_present, '0',
    'Test 10: idle session no longer present in pg_stat_activity, without sending it another command'
);

$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/DETAIL:.*role validity check failed for user "user10": role has passed its VALID UNTIL expiration/,
    'Test 10: server log shows the idle session was terminated in real time, without ever sending it another command'
);

eval { $session10->quit; };

#############################################################################
# Test 6: Credential validation disabled
#
# Kept isolated from the batch above: this flips credential_validation_enabled
# off for the whole server, which would suppress validation for every other
# session in that batch too.
#############################################################################
note "=== Test 6: Credential validation disabled ===";

# Disable credential validation
$node->safe_psql('postgres', "ALTER SYSTEM SET credential_validation_enabled = off;");
$node->reload;

$node->safe_psql('postgres', "CREATE USER user6 LOGIN PASSWORD 'secret6';");
reset_pg_hba($node, 'md5', 'user6');

$ENV{PGPASSWORD} = 'secret6';
my $session6 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user6']
);

# Expire user6
$node->safe_psql('postgres', "ALTER USER user6 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds...";
sleep(7);

# Session should still work since validation is disabled
($stdout, $ret) = $session6->query('SELECT 1 AS validation_disabled;');
like($stdout, qr/1/, 'Test 6: session survives when validation is disabled');

eval { $session6->quit; };

# Re-enable for any subsequent tests
$node->safe_psql('postgres', "ALTER SYSTEM SET credential_validation_enabled = on;");
$node->reload;

#############################################################################
# Test 8: Repeated ERRORs cannot indefinitely delay mandatory revalidation.
#
# Every top-level ERROR cancels all active timeouts (disable_all_timeouts()
# in PostgresMain()), including the credential validation timer.  A client
# that keeps triggering a harmless error more often than the validation
# interval must not be able to use that to postpone revalidation forever.
#############################################################################
note "=== Test 8: repeated ERRORs cannot indefinitely delay revalidation ===";

# Only inspect log content generated from this point on, so this test can't
# be satisfied by a FATAL message left over from an earlier test.
my $test8_log_offset = -s $node->logfile;

$node->safe_psql('postgres', "CREATE USER user8 LOGIN PASSWORD 'secret8b';");
reset_pg_hba($node, 'md5', 'user8');

$ENV{PGPASSWORD} = 'secret8b';
my $session8 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user8']
);

($stdout, $ret) = $session8->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'Test 8: user8 can execute queries initially');

# Expire user8 right away.
$node->safe_psql('postgres', "ALTER USER user8 VALID UNTIL '2020-01-01';");

# Simulate a rogue client firing a harmless error roughly once a second --
# well inside the 5 second validation interval each time -- to try to keep
# resetting the validation timer before it can ever fire.
for (1 .. 8)
{
    eval { $session8->query('SELECT 1/0;'); };
    sleep(1);
}

# Despite the constant stream of errors, the session must still be
# terminated: the original validation deadline must not be pushed back by
# each error's timeout cancellation.
eval {
    ($stdout, $ret) = $session8->query('SELECT 2 AS failure_expected;');
};

$log_contents = slurp_file($node->logfile, $test8_log_offset);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 8: session terminated despite repeated ERRORs attempting to delay validation'
);

eval { $session8->quit; };

#############################################################################
# Test 9: credential_validation_enabled/credential_validation_interval
# cannot be changed within an already-open session (PGC_SU_BACKEND), even by
# a superuser.  This is what closes the gap raised in review: since the
# whole point of this feature is to catch a session whose credentials became
# invalid after authentication, an already-connected session -- including a
# superuser's -- must not be able to silently disable the check on itself to
# dodge its own credential revocation.
#############################################################################
note "=== Test 9: credential_validation_* cannot be changed mid-session (PGC_SU_BACKEND) ===";

my ($psql_ret, $psql_stdout, $psql_stderr);

($psql_ret, $psql_stdout, $psql_stderr) = $node->psql(
    'postgres',
    'SET credential_validation_enabled = off;');
isnt($psql_ret, 0,
    'Test 9: SET credential_validation_enabled fails within a session');
like(
    $psql_stderr,
    qr/cannot be set after connection start/,
    'Test 9: credential_validation_enabled cannot be changed mid-session, even by a superuser'
);

($psql_ret, $psql_stdout, $psql_stderr) = $node->psql(
    'postgres',
    'SET credential_validation_interval = 3600;');
isnt($psql_ret, 0,
    'Test 9: SET credential_validation_interval fails within a session');
like(
    $psql_stderr,
    qr/cannot be set after connection start/,
    'Test 9: credential_validation_interval cannot be changed mid-session, even by a superuser'
);

#############################################################################
# Test 11: a logical replication protocol connection (replication=database)
# can run ordinary SQL, and must be validated like any other session.
#
# EnableCredentialValidationTimeout()/CheckCredentialValidity() used to
# unconditionally skip every walsender (AmWalSenderProcess()), including a
# database-connected (logical) one -- even though, unlike physical
# replication, such a connection is explicitly allowed to run arbitrary SQL
# through the normal command dispatch loop before (or between) replication
# commands. That let a logical-replication-protocol session with expired
# credentials keep running indefinitely. Both functions now only skip
# *physical* replication connections (!am_db_walsender), so this session must
# be terminated exactly like an ordinary one.
#
# This only exercises the pre-streaming phase (running plain SQL over a
# replication=database connection); the equivalent check while actively
# streaming is serviced separately by WalSndHandleCredentialValidation() in
# walsender.c and is not exercised here.
#############################################################################
note "=== Test 11: logical replication connection (replication=database) is validated ===";

$node->safe_psql('postgres',
    "CREATE USER user11 LOGIN REPLICATION PASSWORD 'secret11';");
reset_pg_hba($node, 'md5', 'user11');

$ENV{PGPASSWORD} = 'secret11';
my $session11 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    replication   => 'database',
    extra_params  => ['-U', 'user11']
);

($stdout, $ret) = $session11->query('SELECT 1 AS success;');
like($stdout, qr/1/,
    'Test 11: logical replication connection can run ordinary SQL initially');
is($ret, 0, 'Test 11: no errors during initial query for user11');

$node->safe_psql('postgres', "ALTER USER user11 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for a credential validation cycle...";
sleep(7);

eval {
    ($stdout, $ret) = $session11->query('SELECT 2 AS failure_expected;');
};

$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 11: logical replication session terminated due to expired credentials'
);
like(
    $log_contents,
    qr/DETAIL:.*role validity check failed for user "user11": role has passed its VALID UNTIL expiration/,
    'Test 11: server log DETAIL identifies the user and the VALID UNTIL reason'
);

eval { $session11->quit; };

# Clean up
$node->stop;
done_testing();
