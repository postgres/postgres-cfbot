# Copyright (c) 2026, PostgreSQL Global Development Group

# On Windows, check that a program started by COPY TO PROGRAM doesn't
# inherit handles that keep the server's resources alive after the server
# is gone.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if (!$windows_os)
{
	plan skip_all => 'handle inheritance is specific to Windows';
}

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $ready = "$tempdir/ready";
my $release = "$tempdir/release";

# The program records its PID, then waits until we release it.
my $script = "$tempdir/linger.pl";
append_to_file(
	$script, q{
my ($ready, $release) = @ARGV;
open(my $fh, '>', "$ready.tmp") or die;
print $fh $$;
close($fh);
rename("$ready.tmp", $ready) or die;
for (1 .. 3000)
{
	last if -e $release;
	select(undef, undef, undef, 0.1);
}
});

# Wait for $cond to become true, or give up after the default timeout.
sub wait_until
{
	my ($cond) = @_;

	for (my $i = 0; $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
	{
		return 1 if $cond->();
		usleep(100_000);
	}
	return 0;
}

# If we die early, still let the program go, or it holds up the test run.
END
{
	eval { append_to_file($release, '') } if defined $release;
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
# Listen on TCP even when the tests use Unix-domain sockets, so that there's a
# listen socket whose port we can check.
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$node->start;

my $psql = $node->background_psql('postgres');
$psql->query_until(
	qr/started/, qq{\\echo started
COPY (SELECT 1) TO PROGRAM '"$^X" "$script" "$ready" "$release"';
});

wait_until(sub { -e $ready }) or die "COPY TO PROGRAM did not start";
my $pid = slurp_file($ready);

# This takes a few seconds: the backend is waiting for the program, so it
# doesn't see the shutdown signal, and the postmaster has to kill it.
$node->stop('immediate');

SKIP:
{
	skip 'program did not outlive the server', 3 unless kill(0, $pid);

	# The client socket: the client should see its connection drop.  The
	# timer throws if it expires, so catch that and report a failure.
	$psql->{timeout}->start($PostgreSQL::Test::Utils::timeout_default);
	my $closed = eval {
		pump_until(
			$psql->{run}, $psql->{timeout}, \$psql->{stderr},
			qr/server closed the connection unexpectedly|connection to server was lost/
		);
	};
	ok($closed, 'client connection closed while program is still running');

	# The listen sockets: the port should be free for a new postmaster.
	ok(PostgreSQL::Test::Cluster::can_bind('127.0.0.1', $node->port),
		'port is free while program is still running');

	# The shared memory segment: a new postmaster should start.  The program
	# does still hold the old log file as its stderr, so give the new
	# postmaster another one.
	$node->rotate_logfile;
	ok($node->start(fail_ok => 1), 'restart while program is still running');
}

append_to_file($release, '');
wait_until(sub { !kill(0, $pid) }) or die "program did not exit";

# psql has exited by now, having lost its connection, so don't try \q.
$psql->{run}->finish;

done_testing();
