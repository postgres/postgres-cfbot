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
$node->init;

# Enable credential validation with short interval (5 seconds minimum)
$node->append_conf('postgresql.conf', "credential_validation_enabled = on\n");
$node->append_conf('postgresql.conf', "credential_validation_interval = 5\n");

$node->start;

# Configure password auth for user1 and user2 (must be BEFORE "all all trust")
reset_pg_hba($node, 'md5', 'user1', 'user2');

# Create test users with passwords
$node->safe_psql('postgres', "CREATE USER user1 LOGIN PASSWORD 'secret';");
$node->safe_psql('postgres', "CREATE USER user2 LOGIN PASSWORD 'secret2';");

#############################################################################
# Test 1: VALID UNTIL expiration
#############################################################################
note "=== Test 1: VALID UNTIL expiration ===";

$ENV{PGPASSWORD} = 'secret';
my $session1 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);

# Verify user1 can execute a query normally
my ($stdout, $ret) = $session1->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'user1 can execute queries initially');
is($ret, 0, 'no errors during initial query for user1');

# Admin alters the VALID UNTIL date to the past
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL '2025-11-02 16:59:37+05:30';");

# Wait for the credential validation timeout to fire
note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

# User1 attempts to execute another query - should be terminated
eval {
    ($stdout, $ret) = $session1->query('SELECT 2 AS failure_expected;');
};

# Check the server log for the expected FATAL error
my $log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 1: server log shows session terminated due to expired credentials'
);

eval { $session1->quit; };

#############################################################################
# Test 2: User dropped while session is active
#############################################################################
note "=== Test 2: User dropped while session is active ===";

$ENV{PGPASSWORD} = 'secret2';
my $session2 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user2']
);

# Verify user2 can execute a query normally
($stdout, $ret) = $session2->query('SELECT 1 AS success;');
like($stdout, qr/1/, 'user2 can execute queries initially');
is($ret, 0, 'no errors during initial query for user2');

# Admin drops user2 while the session is still active
$node->safe_psql('postgres', "DROP USER user2;");

# Wait for the credential validation timeout to fire
note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

# User2 attempts to execute another query - should be terminated
eval {
    ($stdout, $ret) = $session2->query('SELECT 2 AS failure_expected;');
};

# Check the server log for the expected FATAL error (user no longer exists)
$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 2: server log shows session terminated after user was dropped'
);

eval { $session2->quit; };

#############################################################################
# Test 3: VALID UNTIL extended keeps session alive (positive test)
#############################################################################
note "=== Test 3: VALID UNTIL extended keeps session alive ===";

# Reset user1 for this test (user1 still exists from Test 1, just expired)
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL 'infinity';");
reset_pg_hba($node, 'md5', 'user1');

$ENV{PGPASSWORD} = 'secret';
my $session3 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);

# Set VALID UNTIL to far future
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL '2099-12-31 23:59:59';");

# Wait for validation cycle
note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

# Session should still be alive
($stdout, $ret) = $session3->query('SELECT 1 AS still_alive;');
like($stdout, qr/1/, 'Test 3: session remains alive with valid VALID UNTIL');
is($ret, 0, 'Test 3: no errors when VALID UNTIL is in the future');

eval { $session3->quit; };

#############################################################################
# Test 4: Multiple sessions terminated when user expires
#############################################################################
note "=== Test 4: Multiple sessions terminated when user expires ===";

# Reset user1
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL 'infinity';");

$ENV{PGPASSWORD} = 'secret';
my $session4a = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);
my $session4b = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);

# Verify both sessions work
($stdout, $ret) = $session4a->query('SELECT 1;');
like($stdout, qr/1/, 'session4a works initially');
($stdout, $ret) = $session4b->query('SELECT 1;');
like($stdout, qr/1/, 'session4b works initially');

# Expire user1
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

# Both sessions should fail
eval { $session4a->query('SELECT 2;'); };
eval { $session4b->query('SELECT 2;'); };

$log_contents = slurp_file($node->logfile);
# Count occurrences of the termination message
my @matches = ($log_contents =~ /FATAL:.*session credentials have expired/g);
cmp_ok(scalar(@matches), '>=', 3, 'Test 4: multiple sessions terminated for same user');

eval { $session4a->quit; };
eval { $session4b->quit; };

#############################################################################
# Test 5: Trust auth sessions are not affected
#############################################################################
note "=== Test 5: Trust auth sessions are not affected ===";

# Create user3 with trust auth (no password validation registered)
$node->safe_psql('postgres', "CREATE USER user3 LOGIN;");
reset_pg_hba($node, 'trust', 'user3');

delete $ENV{PGPASSWORD};
my $session5 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user3']
);

# Set expired VALID UNTIL (but trust auth has no validator)
$node->safe_psql('postgres', "ALTER USER user3 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

# Session should still work - trust has no registered validator
($stdout, $ret) = $session5->query('SELECT 1 AS trust_still_works;');
like($stdout, qr/1/, 'Test 5: trust auth session not terminated (no validator)');

eval { $session5->quit; };

#############################################################################
# Test 6: Credential validation disabled
#############################################################################
note "=== Test 6: Credential validation disabled ===";

# Disable credential validation
$node->safe_psql('postgres', "ALTER SYSTEM SET credential_validation_enabled = off;");
$node->reload;

# Reset user1
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL 'infinity';");
reset_pg_hba($node, 'md5', 'user1');

$ENV{PGPASSWORD} = 'secret';
my $session6 = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'user1']
);

# Expire user1
$node->safe_psql('postgres', "ALTER USER user1 VALID UNTIL '2020-01-01';");

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
# Test 7: SET SESSION AUTHORIZATION does not change whose credentials are
# checked.  Regression test for using GetAuthenticatedUserId() (the role
# that actually authenticated) rather than GetSessionUserId() (the mutable
# current session role) in the baseline role-validity check.
#############################################################################
note "=== Test 7: SET SESSION AUTHORIZATION does not change whose credentials are checked ===";

# authsuperN logs in over the wire, so its credentials are what actually got
# authenticated.  sesstargetN is only ever reached via SET SESSION
# AUTHORIZATION, never over the wire, so it needs no pg_hba entry or
# password of its own.
$node->safe_psql('postgres', "CREATE USER authsuper1 LOGIN SUPERUSER PASSWORD 'secret7';");
$node->safe_psql('postgres', "CREATE ROLE sesstarget1;");
$node->safe_psql('postgres', "CREATE USER authsuper2 LOGIN SUPERUSER PASSWORD 'secret8';");
$node->safe_psql('postgres', "CREATE ROLE sesstarget2;");
reset_pg_hba($node, 'md5', 'authsuper1', 'authsuper2');

#############################################################################
# Test 7a: the *originally authenticated* role's expiry still terminates the
# session even after switching to a still-valid session role.  If the
# baseline check regressed to GetSessionUserId(), this session would survive
# instead (sesstarget1 is never touched), so this test would catch that.
#############################################################################
note "--- Test 7a: original login role expiring still terminates session after SET SESSION AUTHORIZATION ---";

$ENV{PGPASSWORD} = 'secret7';
my $session7a = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'authsuper1']
);

($stdout, $ret) = $session7a->query('SET SESSION AUTHORIZATION sesstarget1;');
is($ret, 0, 'Test 7a: SET SESSION AUTHORIZATION succeeds for superuser');

($stdout, $ret) = $session7a->query('SELECT current_user;');
like($stdout, qr/sesstarget1/, 'Test 7a: session is now running as sesstarget1');

# Expire the ORIGINAL login role (authsuper1), not the current session role.
$node->safe_psql('postgres', "ALTER USER authsuper1 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

eval {
    ($stdout, $ret) = $session7a->query('SELECT 2 AS failure_expected;');
};

$log_contents = slurp_file($node->logfile);
like(
    $log_contents,
    qr/FATAL:.*session credentials have expired/,
    'Test 7a: session terminated on original login role expiry, despite SET SESSION AUTHORIZATION to a still-valid role'
);

eval { $session7a->quit; };

#############################################################################
# Test 7b: the *current session role's* own expiry is irrelevant -- only the
# originally authenticated role is checked.  If the baseline check regressed
# to GetSessionUserId(), this session would be wrongly terminated instead of
# surviving, so this test would catch that.
#############################################################################
note "--- Test 7b: session role's own expiry is irrelevant after SET SESSION AUTHORIZATION ---";

$ENV{PGPASSWORD} = 'secret8';
my $session7b = $node->background_psql(
    'postgres',
    on_error_stop => 0,
    extra_params  => ['-U', 'authsuper2']
);

($stdout, $ret) = $session7b->query('SET SESSION AUTHORIZATION sesstarget2;');
is($ret, 0, 'Test 7b: SET SESSION AUTHORIZATION succeeds for superuser');

($stdout, $ret) = $session7b->query('SELECT current_user;');
like($stdout, qr/sesstarget2/, 'Test 7b: session is now running as sesstarget2');

# Expire the CURRENT session role (sesstarget2); authsuper2, the role that
# actually authenticated, is left untouched (still valid).
$node->safe_psql('postgres', "ALTER ROLE sesstarget2 VALID UNTIL '2020-01-01';");

note "Waiting 7 seconds for credential validation timeout to fire...";
sleep(7);

($stdout, $ret) = $session7b->query('SELECT 1 AS still_alive;');
like($stdout, qr/1/, 'Test 7b: session survives when only the session role (not the login role) has expired');
is($ret, 0, 'Test 7b: no errors -- validation checks the authenticated role, not the session role');

eval { $session7b->quit; };

# Clean up
$node->stop;
done_testing();
