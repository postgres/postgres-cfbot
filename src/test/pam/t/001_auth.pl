
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use File::Copy;
use File::Basename;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_pam} ne 'yes')
{
	plan skip_all => 'PAM not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bpam\b/)
{
	plan skip_all =>
	  'Potentially unsafe test PAM not enabled in PG_TEST_EXTRA';
}
elsif (!check_pg_config("#define HAVE_PAM_START_CONFDIR 1"))
{
	plan skip_all =>
	  'PAM tests requires Postgres build with pam library that supports pam_start_confdir function';
}

note "setting up PostgreSQL instance";

# need to use IP to test correct transmission of PAM_RHOST
my $hostaddr = '127.0.0.1';

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '$hostaddr'
log_connections = all
log_min_messages = debug2
	});
$node->start;

$node->safe_psql('postgres', 'CREATE USER test0;');
$node->safe_psql('postgres', 'CREATE USER test1;');
$node->safe_psql('postgres', 'CREATE USER test2;');
$node->safe_psql('postgres', 'CREATE USER test3;');
$node->safe_psql('postgres', 'CREATE USER test4;');
$node->safe_psql('postgres', 'CREATE USER test5;');
$node->safe_psql('postgres', 'CREATE USER test6;');

note "running tests";

my $test_temp = PostgreSQL::Test::Utils::tempdir("pam-001_auth");
my $test_pam_conf_pg = "$test_temp/postgresql";
my $test_pam_conf_pg2 = "$test_temp/postgresql2";
my $test_pam_conf_pg3 = "$test_temp/postgresql3";
# postgresql4 intentionally unused for negative test
my $test_pam_conf_pg5 = "$test_temp/postgresql5";

my $test_pam_exec_dir = PostgreSQL::Test::Utils::tempdir("001_authpam_exec");
my $test_pam_exec = "$test_pam_exec_dir/user.sh";
my $test_pam_exec_log = "${PostgreSQL::Test::Utils::tmp_check}/log/pam_exec.log";

# pam_exec script will test all relevant info sent to PAM.
# user/password is intentionally checked last
append_to_file($test_pam_exec, qq{#! /bin/sh

set -x

read pam_passwd

if [ "\$PAM_TTY" != "" ]; then
    exit 1
elif [ "\$PAM_TYPE" != "auth" ]; then
    exit 2
elif [ "\$PAM_RHOST" != "127.0.0.1" ]; then
    exit 3
elif [ "\$PAM_SERVICE" != "postgresql3" ]; then
    exit 4
elif [ "\$PAM_RUSER" != "" ]; then
    exit 5
elif [ "\$PAM_USER" != "test3" ]; then
    exit 6
elif [ "\$pam_passwd" != "password3" ]; then
    exit 7
fi
exit 0
});

# Should allow all connections
append_to_file($test_pam_conf_pg, qq{
auth   required pam_permit.so
account  required pam_permit.so
});

# Should deny all connections
append_to_file($test_pam_conf_pg2, qq{
auth   required pam_deny.so
account  required pam_deny.so
});

# will route to sh script to test password/username/host transmission
append_to_file($test_pam_conf_pg3, qq{
auth   required pam_exec.so debug expose_authtok log=$test_pam_exec_log /bin/sh $test_pam_exec
account  required pam_permit.so
});

# postgresql4 intentionally unused for negative test

# Should allow authenticaton but not account, connections should fail
append_to_file($test_pam_conf_pg5, qq{
auth   required pam_permit.so
account  required pam_deny.so
});

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
local all test0 pam pamconfdir=$test_temp
local all test1 pam pamconfdir=$test_temp pamservice=postgresql
local all test2 pam pamconfdir=$test_temp pamservice=postgresql2
host all test3 $hostaddr/32 pam pamconfdir=$test_temp pamservice=postgresql3
host all test4 $hostaddr/32 pam pamconfdir=$test_temp pamservice=postgresql3
local all test5 pam pamconfdir=$test_temp pamservice=postgresql4
local all test6 pam pamconfdir=$test_temp pamservice=postgresql5
});
$node->restart;

$node->connect_ok("user=test0",
	"correctly routes pam.d/postgresql by default, connection succeds due to pam_accept.so",
	log_like => [qr/LOG:  connection authenticated: identity="test0" method=pam/],
);

$node->connect_ok("user=test1",
	"correctly routes pam.d/postgresql by explicit, connection succeds due to pam_accept.so",
	log_like => [qr/LOG:  connection authenticated: identity="test1" method=pam/],
);

$node->connect_fails("user=test2",
	"correctly routes to pam.d/postgresql2, connection fails due to pam_deny.so",
	expected_stderr => qr/FATAL:  PAM authentication failed for user "test2"/,
	log_like => [qr/LOG:  pam_authenticate failed: Authentication failure/],
);

$node->connect_ok("user=test3 password=password3 host=$hostaddr",
	"correctly routes pam.d/postgresql3, accepted",
	log_like => [qr/LOG:  connection authenticated: identity="test3" method=pam/],
);

$node->connect_fails("user=test3 password=wrongpassword3 host=$hostaddr",
	"correctly routes to pam.d/postgresql3, rejected due to wrong password",
	expected_stderr => qr/FATAL:  PAM authentication failed for user "test3"/,
	log_like => [
		qr/LOG:  error from underlying PAM layer: \/bin\/sh failed: exit code 7/,
		qr/LOG:  pam_authenticate failed: System error/,
	],
);

$node->connect_fails("user=test4 password=wrongpassword3 host=$hostaddr",
	"correctly routes to pam.d/postgresql3, rejected due to wrong user",
	expected_stderr => qr/FATAL:  PAM authentication failed for user "test4"/,
	log_like => [
		qr/LOG:  error from underlying PAM layer: \/bin\/sh failed: exit code 6/,
		qr/LOG:  pam_authenticate failed: System error/,
	],
);

$node->connect_fails("user=test5 password=wrongpassword3",
	"routes to non existent pamservice 'postgresql5' pam_start_confdir call fails",
	expected_stderr => qr/FATAL:  PAM authentication failed for user "test5"/,
	log_like => [qr/LOG:  could not create PAM authenticator: Critical error - immediate abort/],
);

$node->connect_fails("user=test6 password=wrongpassword3",
	"routes to non existent pamservicer6 which should pass auth but fail account",
	expected_stderr => qr/FATAL:  PAM authentication failed for user "test6"/,
	log_like => [qr/LOG:  pam_acct_mgmt failed: Authentication failure/],
);

$node->teardown_node;

done_testing();
