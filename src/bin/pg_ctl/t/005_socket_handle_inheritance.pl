# Copyright (c) 2025, PostgreSQL Global Development Group

# Test that socket handles are not inherited by child processes on Windows.
#
# Without the fix, child processes spawned via COPY TO PROGRAM inherit socket
# handles from the backend. Windows reference counting prevents these sockets
# from being freed when the postmaster exits, leaving the port bound to a dead
# process (a "zombie" binding). This test verifies that after killing the
# postmaster while a child process is still running, the listening port is
# immediately freed rather than remaining in a zombie state.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(sleep);

# This test is Windows-specific
if ($^O ne 'MSWin32')
{
	plan skip_all => 'test is specific to Windows socket handle inheritance';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Get the port number for verification
my $port = $node->port;

# Spawn a long-running child process via COPY TO PROGRAM that will outlive
# the postmaster. Without the fix, this child inherits socket handles.
my $marker_file = $node->data_dir . '/ps_marker.txt';
unlink $marker_file if -e $marker_file;

$node->safe_psql(
	'postgres',
	qq{\\copy (select 1) to program 'powershell -Command "echo marker > $marker_file; Start-Sleep 120"'}
);

# Wait for PowerShell to spawn
my $ps_spawned = 0;
for (my $i = 0; $i < 100; $i++)
{
	if (-e $marker_file)
	{
		$ps_spawned = 1;
		last;
	}
	sleep 0.1;
}

ok($ps_spawned, 'child process spawned successfully');

# Stop the postmaster (simulates a crash), leaving the child process running.
$node->stop('immediate');
sleep 0.5;

# Verify that the listening port is freed immediately. With the bug, the port
# remains bound to the dead postmaster PID because the child process inherited
# the socket handles. With the fix, the port is freed because socket handles
# were not inherited.
my $netstat_output = `netstat -ano | findstr ":$port.*LISTENING"`;

if ($netstat_output)
{
	fail('listening port remains bound after postmaster exit (zombie port)');
	diag("Port is still bound - socket handles were inherited by child process");
	diag("netstat output:\n$netstat_output");

	if ($netstat_output =~ /LISTENING\s+(\d+)/)
	{
		my $bound_pid = $1;
		my $process_name = get_process_name($bound_pid);

		if ($process_name eq 'unknown' || $process_name eq '')
		{
			diag("Port bound to dead process (PID $bound_pid) - zombie binding detected");
		}
		else
		{
			diag("Port bound to: $process_name (PID $bound_pid)");
		}
	}
}
else
{
	pass('listening port freed immediately after postmaster exit');
}

# Additional verification: Confirm the port is actually available for binding.
# This tests the real-world scenario that matters to users.
my $can_bind = test_port_available($port);
ok($can_bind, "port $port is available for new connections");

# Cleanup
cleanup_powershell_processes();
unlink $marker_file if -e $marker_file;

done_testing();

# Test if port can actually be bound
sub test_port_available
{
	my ($port) = @_;

	use Socket;

	socket(my $sock, PF_INET, SOCK_STREAM, getprotobyname('tcp')) or return 0;
	setsockopt($sock, SOL_SOCKET, SO_REUSEADDR, 1);

	my $addr = sockaddr_in($port, INADDR_ANY);
	my $result = bind($sock, $addr);
	close($sock);

	return $result ? 1 : 0;
}

# Get process name by PID
sub get_process_name
{
	my ($pid) = @_;
	my $name = `powershell -Command "(Get-Process -Id $pid -ErrorAction SilentlyContinue).ProcessName" 2>nul`;
	chomp $name;
	return $name || 'unknown';
}

# Clean up test child processes
sub cleanup_powershell_processes
{
	system('powershell -Command "Get-Process powershell -ErrorAction SilentlyContinue | Where-Object {$_.Id -ne $PID} | Stop-Process -Force -ErrorAction SilentlyContinue" 2>nul');
}