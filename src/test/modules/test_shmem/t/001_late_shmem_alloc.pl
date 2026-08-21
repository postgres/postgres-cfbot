# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###
# Test allocating memory after startup, i.e. when the library is not
# in shared_preload_libraries
###
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Test a failure in initialization of the shared memory area.
SKIP:
{
	skip "injection points not supported by this build",
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql("postgres", "CREATE EXTENSION injection_points;");
	$node->safe_psql("postgres",
		"SELECT injection_points_attach('test-shmem-init', 'error');");

	# A failure in the requesting shared memory should not affect server
	# availability. The session should remain useful, however trying to create
	# the same extension again will fail.
	my (undef, undef, $stderr) =
	  $node->psql("postgres", "CREATE EXTENSION test_shmem;");
	like($stderr,
		qr/error triggered for injection point test-shmem-init/,
		"failure in initialization is reported");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('test-shmem-init');");
	(undef, undef, $stderr) =
	  $node->psql("postgres", "CREATE EXTENSION test_shmem;");
	like($stderr, qr/shmem area not yet initialized/,
		"post-init extension creation fails");

	# Only a server restart can remove the partially allocated shared memory
	# area.
	$node->restart;
}

# Test failure when the request is larger than the memory reserved for
# after-startup requests.
my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query(q[SET test_shmem.area_size = '128kB';], verbose => 0);
$session->query("CREATE EXTENSION test_shmem;", verbose => 0);
like($session->{stderr}, qr/not enough shared memory/,
	"an after-startup request larger than the reserve fails");

# The server and the backend should still be available. Since there was only one
# area requested, the failure did not change anything in the shared memory.
# Verify that the request for smaller area succeeds in the same session.
$session->{stderr} = '';
$session->query("SET test_shmem.area_size = default;", verbose => 0);
$session->query_safe("CREATE EXTENSION test_shmem;", verbose => 0);
$session->quit;

# Check that the attach counter is incremented on a new connection
my $attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1,
	"attach callback is called in each backend");
$node->stop;

###
# Test that loading via shared_preload_libraries also works, even for large request.
###
$node->append_conf('postgresql.conf', "test_shmem.area_size = '128kB'");
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_shmem'");
$node->start;

# When loaded via shared_preload_libraries, the attach callback is
# called or not, depending on whether this is an EXEC_BACKEND build.
my $exec_backend =
  $node->safe_psql("postgres", "SHOW debug_exec_backend;") eq 'on';
$attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
$attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");

if ($exec_backend)
{
	cmp_ok($attach_count2, '>', $attach_count1,
		"attach callback is called in each backend when loaded via shared_preload_libraries"
	);
}
else
{
	ok( $attach_count1 == 0 && $attach_count2 == 0,
		"attach callback is not called when loaded via shared_preload_libraries"
	);
}

$node->stop;
done_testing();
