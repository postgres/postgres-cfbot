# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test resizable shared memory functionality, both when loaded at startup via
# shared_preload_libraries and when loaded after startup (late allocation).

# Verify that enough shared memory is allocated to cover the resizable_shmem
# structure at its current size but does not exceed the memory required by the
# current sizes of all shared memory structures. We expect that the backend
# where we run the query will have touched the entire resizable_shmem structure,
# so that all the memory pages covering the resizable structure are mapped to
# the backend's address space.
#
# Since we have configured the server so that resizable shared struture
# dominates the main shared memory segment, the total memory allocated to other
# shared memory structures does not result in false positive tests below.
sub check_shmem_usage
{
	my ($session, $label, $node) = @_;

	my $shmem_usage = $session->query_safe('SELECT test_shmem_usage();', verbose => 0);
	my $total_alloc = $node->safe_psql('postgres', "SELECT sum(allocated_size) FROM pg_shmem_allocations;");
	my $resizable_alloc = $node->safe_psql('postgres',
											"SELECT allocated_size FROM pg_shmem_allocations WHERE name = 'resizable_shmem';");

	diag "$label: shmem_usage=$shmem_usage, resizable_shmem allocated=$resizable_alloc, sum(allocated_size)=$total_alloc";
	ok($shmem_usage <= $total_alloc,
	   "$label: allocated shared memory does not exceed total allocated size");
	ok($shmem_usage >= $resizable_alloc,
	   "$label: shared memory usage covers the resizable_shmem allocation");
}

# Test a resize operation: resize, verify old data, write new data, verify
# new data, and check shmem usage.  Returns updated ($num_entries, $value).
sub test_resize
{
	my ($node, $prefix, $old_num_entries, $old_value, $new_num_entries, $new_value, $label) = @_;

	$label = "$prefix: $label";

	my $session1 = $node->background_psql('postgres');
	my $session2 = $node->background_psql('postgres');

	$session1->query_safe("SELECT resizable_shmem_resize($new_num_entries);",
						  verbose => 0);

	# Old data should still be intact in the (possibly smaller) area
	my $readable_entries = ($new_num_entries < $old_num_entries) ? $new_num_entries : $old_num_entries;
	is($session1->query_safe("SELECT resizable_shmem_read($readable_entries, $old_value);",
							 verbose => 0),
	   't', "old data readable after $label");

	$session2->query_safe("SELECT resizable_shmem_write($new_value);",
						  verbose => 0);
	is($session1->query_safe("SELECT resizable_shmem_read($new_num_entries, $new_value);",
							 verbose => 0),
	   't', "new data readable after $label");

	check_shmem_usage($session1, "$label (session 1)", $node);
	check_shmem_usage($session2, "$label (session 2)", $node);

	$session1->quit;
	$session2->quit;

	return ($new_num_entries, $new_value);
}

# Verify that reads or writes past the current size, but within the reserved
# maximum, fault when they reach the protected region.
sub test_fault_beyond_size
{
	my ($node, $initial_entries, $prefix) = @_;

	# Enable restart_after_crash to test postmaster driven restart with
	# resizable shared memory.
	$node->safe_psql('postgres',
		'ALTER SYSTEM SET restart_after_crash = on;');
	$node->reload;

	for my $mode ('write', 'read')
	{
		my ($ret, $stdout, $stderr) = $node->psql('postgres',
			"SELECT resizable_shmem_access_beyond_size('$mode');");
		ok($ret != 0, "$prefix: $mode past current size crashes the backend");
		like($stderr,
			 qr/server closed the connection unexpectedly|connection to server was lost/,
			 "$prefix: $mode crash reports lost connection");

		$node->poll_query_until('postgres', 'SELECT 1', '1')
		  or die "server did not come back after $mode crash";
	}

	is($node->safe_psql('postgres',
			"SELECT resizable_shmem_read($initial_entries, 0);"),
	   't', "$prefix: read succeeds after crash recovery");

	$node->safe_psql('postgres', 'ALTER SYSTEM RESET restart_after_crash;');
	$node->reload;
}

# Run the full suite of resizable shared memory tests on the given node.
sub run_resizable_tests
{
	my ($node, $initial_entries, $max_entries, $prefix) = @_;
	my $have_resizable_shmem = $node->safe_psql('postgres', 'SHOW have_resizable_shmem;') eq 'on';

	my $num_entries = $initial_entries;

	# Basic read/write should work on all platforms
	my $value = 100;
	$node->safe_psql('postgres', "SELECT resizable_shmem_write($value);");
	is($node->safe_psql('postgres', "SELECT resizable_shmem_read($num_entries, $value);"),
	   't', "$prefix: data read after write successful");

	if ($have_resizable_shmem)
	{
		# Initial structure state
		my $session1 = $node->background_psql('postgres');
		my $session2 = $node->background_psql('postgres');

		$value = 100;
		# Write and read the initial set of entries.
		$session1->query_safe("SELECT resizable_shmem_write($value);", verbose => 0);
		is($session2->query_safe("SELECT resizable_shmem_read($num_entries, $value);",
								 verbose => 0),
		   't', "$prefix: data read after write successful");
		check_shmem_usage($session1, "$prefix: initial write (session 1)", $node);
		check_shmem_usage($session2, "$prefix: initial write (session 2)", $node);
		$session1->quit;
		$session2->quit;

		# Verify no other structure is resizable
		is($node->safe_psql('postgres', "SELECT count(*) FROM pg_shmem_allocations WHERE name <> 'resizable_shmem' AND maximum_size <> minimum_size;"),
							'0', "$prefix: no other resizable structures");

		# Resize to maximum
		($num_entries, $value) = test_resize($node, $prefix, $num_entries, $value,
											 $max_entries, 500, 'resize to maximum');

		# Shrink to 75% of max
		my $shrink_entries = int($max_entries * 3 / 4);
		($num_entries, $value) = test_resize($node, $prefix, $num_entries, $value,
											 $shrink_entries, 999, 'shrinking');

		# Resize to the same size (no-op)
		($num_entries, $value) = test_resize($node, $prefix, $num_entries, $value,
											 $num_entries, 1999, 'no-op resize');

		# Shrink to minimum i.e. zero entries and grow back
		($num_entries, $value) = test_resize($node, $prefix, $num_entries, $value,
											 0, 0, 'shrink to minimum');
		($num_entries, $value) = test_resize($node, $prefix, $num_entries, $value,
											 $initial_entries, 2999,
											 'grow back from minimum');

		# Test resize failure (attempt to resize beyond max - should fail)
		my ($ret, $stdout, $stderr) =
			$node->psql('postgres', "SELECT resizable_shmem_resize(" . ($max_entries * 2) . ");");
		ok($ret != 0 || $stderr =~ /ERROR/, "$prefix: Resize beyond maximum fails");

		# Resize to a size below minimum_size must fail.
		($ret, $stdout, $stderr) =
			$node->psql('postgres', 'SELECT resizable_shmem_resize(-1);');
		ok($ret != 0, "$prefix: resize below minimum_size fails");
		like($stderr,
			 qr/cannot shrink shared memory structure "resizable_shmem" below minimum size/,
			 "$prefix: resize-below-minimum error comes from ShmemResizeStruct");

		# The fault test relies on a hole being present between the current end
		# of the structure and its maximal end. Skip when the structure does not
		# span multiple pages.
		my $spans_pages = $node->safe_psql('postgres', qq{
			SELECT (maximum_size - minimum_size) >= test_shmem_pagesize()
			FROM pg_shmem_allocations WHERE name = 'resizable_shmem';
		});
		if ($spans_pages ne 't')
		{
			diag "$prefix: skipping fault-beyond-size test: resizable_shmem does not span multiple shmem pages";
		}
		else
		{
			test_fault_beyond_size($node, $initial_entries, $prefix);
		}
	}
	else
	{
		# On unsupported platforms, resizing should fail with a clear error
		my ($ret, $stdout, $stderr) =
			$node->psql('postgres', "SELECT resizable_shmem_resize($num_entries);");
		ok($ret != 0, "$prefix: resize fails on unsupported platform");
		like($stderr, qr/not supported/, "$prefix: resize error mentions not supported");
	}
}

# Check the runtime-computed shared_memory_{initial,minimum,maximum}_size GUC
# invariants.  min <= initial <= max must always hold.  When a resizable
# structure has been registered on a server that supports resizable shared
# memory structures, min must additionally be strictly less than max;
# otherwise all three GUCs must be equal.
sub check_shmem_size_gucs
{
	my ($node, $label) = @_;
	my $pgdata = $node->data_dir;

	my $get = sub {
		my ($guc) = @_;
		my ($stdout, $stderr) = run_command([ 'postgres', '-D' => $pgdata, '-C' => $guc ]);

		return $stdout;
	};

	my $have_resizable_shmem = $get->('have_resizable_shmem');
	my $ini = 0 + $get->('shared_memory_initial_size');
	my $min = 0 + $get->('shared_memory_minimum_size');
	my $max = 0 + $get->('shared_memory_maximum_size');
	my $have_resizable_struct = ($get->('resizable_shmem.max_entries') ne '');

	ok($min <= $ini && $ini <= $max, "$label: shared_memory size GUCs in expected order");

	if ($have_resizable_struct && $have_resizable_shmem eq 'on')
	{
		ok($min < $max, "$label: min < max with resizable structures");
	}
	else
	{
		ok($min == $ini && $ini == $max,
		   "$label: all shared_memory size GUCs equal when no resizable structures");
	}
}

# Log the runtime shared_memory_{initial,minimum,maximum}_size GUCs and huge
# pages usage information for easier debugging.
sub diag_shmem_sizes
{
	my ($node, $label) = @_;

	my $vals = $node->safe_psql('postgres', q{
		SELECT format('initial=%s minimum=%s maximum=%s huge_pages_status=%s huge_page_size=%s shmem_page_size=%s',
					  current_setting('shared_memory_initial_size'),
					  current_setting('shared_memory_minimum_size'),
					  current_setting('shared_memory_maximum_size'),
					  current_setting('huge_pages_status'),
					  current_setting('huge_page_size'),
					  test_shmem_pagesize());
	});
	diag "$label: $vals";
}

### Set up a test node.
#
# Configure minimal shared memory so that the resizable_shmem structure dominates
# and any unexpected increase is easy to detect.
#
# If we turn on huge pages and the machine where the test is running does not
# have huge pages available, the test will fail midway because it will not be
# able to allocate memory pages when expanding the resizable_shmem structure.
# Hence we turn off huge pages for this test. The test outputs the GUCs
# shared_memory_{initial,minimum,maximum}_size and information about huge pages.
# By provisioning enough huge pages, and by changing huge_pages = try/on, the
# test can be run with huge pages enabled.
###
my $node = PostgreSQL::Test::Cluster->new('resizable_shmem');
$node->init;

$node->append_conf('postgresql.conf', 'huge_pages = off');
$node->append_conf('postgresql.conf', 'shared_buffers = 128kB');
$node->append_conf('postgresql.conf', 'max_connections = 5');
$node->append_conf('postgresql.conf', 'max_worker_processes = 0');
$node->append_conf('postgresql.conf', 'max_wal_senders = 0');
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 0');
$node->append_conf('postgresql.conf', 'max_locks_per_transaction = 10');
$node->append_conf('postgresql.conf', 'max_pred_locks_per_transaction = 10');
$node->append_conf('postgresql.conf', 'wal_buffers = 32kB');

###
# Test 1: Startup allocation via shared_preload_libraries
###
my $startup_initial = 25 * 1024 * 1024;
my $startup_max = 100 * 1024 * 1024;

$node->append_conf('postgresql.conf', 'shared_preload_libraries = test_shmem');
$node->append_conf('postgresql.conf', "resizable_shmem.initial_entries = $startup_initial");
$node->append_conf('postgresql.conf', "resizable_shmem.max_entries = $startup_max");

check_shmem_size_gucs($node, 'startup preload');

$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_shmem;');
diag_shmem_sizes($node, 'startup');
run_resizable_tests($node, $startup_initial, $startup_max, 'startup');

my $have_resizable_shmem = $node->safe_psql('postgres', 'SHOW have_resizable_shmem;') eq 'on';

###
# Test 2: Late allocation (loaded after startup, not in shared_preload_libraries).
# Use much smaller sizes since only ~100KB of shared memory is available for
# structures allocated after startup.
###
my $late_initial = 5 * 1024;
my $late_max = 12 * 1024;

$node->safe_psql('postgres', qq{
	ALTER SYSTEM RESET shared_preload_libraries;
	ALTER SYSTEM SET resizable_shmem.initial_entries = $late_initial;
	ALTER SYSTEM SET resizable_shmem.max_entries = $late_max;
});
$node->safe_psql('postgres', 'DROP EXTENSION test_shmem;');
$node->restart;

$node->safe_psql('postgres', 'CREATE EXTENSION test_shmem;');
diag_shmem_sizes($node, 'late');
run_resizable_tests($node, $late_initial, $late_max, 'late');

###
# Test sysv shared memory does not support resizable shmem.  Only relevant on
# platforms that support resizable shmem (HAVE_RESIZABLE_SHMEM), since the
# module only sets maximum_size in that case.
###
if ($have_resizable_shmem)
{
	###
	# Test 3: Verify that CREATE EXTENSION fails with sysv shared memory
	# when loaded after startup (not in shared_preload_libraries).
	###
	$node->safe_psql('postgres', 'DROP EXTENSION test_shmem;');

	# Remove settings that would cause the library to auto-load at startup:
	# shared_preload_libraries and module-prefixed GUCs.  ALTER SYSTEM RESET
	# only affects postgresql.auto.conf, so we must use adjust_conf to remove
	# from postgresql.conf.
	$node->adjust_conf('postgresql.conf', 'shared_preload_libraries', undef);
	$node->adjust_conf('postgresql.conf', 'resizable_shmem.initial_entries', undef);
	$node->adjust_conf('postgresql.conf', 'resizable_shmem.max_entries', undef);
	$node->adjust_conf('postgresql.auto.conf', 'shared_preload_libraries', undef);
	$node->adjust_conf('postgresql.auto.conf', 'resizable_shmem.initial_entries', undef);
	$node->adjust_conf('postgresql.auto.conf', 'resizable_shmem.max_entries', undef);
	$node->safe_psql('postgres', qq{
		ALTER SYSTEM SET shared_memory_type = 'sysv';
	});

	$node->stop;

	check_shmem_size_gucs($node, 'sysv');

	$node->start;

	is($node->safe_psql('postgres', 'SHOW have_resizable_shmem;'),
	   'off',
	   'have_resizable_shmem reports off with shared_memory_type = sysv');

	my ($ret, $stdout, $stderr) =
		$node->psql('postgres', 'CREATE EXTENSION test_shmem;');
	ok($ret != 0, 'CREATE EXTENSION fails with resizable shmem on sysv');
	like($stderr, qr/resizable shared memory requires shared_memory_type = mmap/,
		'CREATE EXTENSION error mentions shared_memory_type = mmap requirement');

	###
	# Test 4: Verify that resizable structures are also rejected with sysv
	# shared memory when loaded at startup via shared_preload_libraries.
	###
	$node->safe_psql('postgres', qq{
		ALTER SYSTEM SET shared_preload_libraries = 'test_shmem';
		ALTER SYSTEM SET resizable_shmem.initial_entries = $startup_initial;
		ALTER SYSTEM SET resizable_shmem.max_entries = $startup_max;
	});
	$node->stop;

	ok(!$node->start(fail_ok => 1),
		'server fails to start with resizable shmem on sysv');

	my $log = slurp_file($node->logfile);
	like($log, qr/resizable shared memory requires shared_memory_type = mmap/,
		'log mentions shared_memory_type = mmap requirement');
}

done_testing();
