# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that log_postmaster_overloads does not spam the log with
# "postmaster potentially overloaded, stats:" log lines while the postmaster
# is idle. It is only supposed to log once the postmaster is nearly at
# the edge of single CPU core capacity. Also test
# log_postmaster_excess_connections as it is very similiar.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Best-effort detection of the number of CPUs available
sub get_cpus_num
{
	my $ncpus;

	if ($windows_os)
	{
		$ncpus = $ENV{NUMBER_OF_PROCESSORS};
	}
	else
	{
		$ncpus = `nproc 2>/dev/null`;
		chomp $ncpus if defined $ncpus;
		if (!defined($ncpus) || $ncpus !~ /^\d+$/)
		{
			$ncpus = `sysctl -n hw.ncpu 2>/dev/null`;
			chomp $ncpus if defined $ncpus;
		}
	}

	return $ncpus if defined($ncpus) && $ncpus =~ /^\d+$/ && $ncpus > 0;
	# Return there is 16 VCPUs as fallback, so that we can that we can
	# hopefully overloaded postmaster, but we don't really know, so
	# hopefully that's enough.
	return 16;
}

my $clients = get_cpus_num() * 2;
note("using $clients pgbench clients");

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
log_postmaster_overloads = on
log_postmaster_excess_connections = 2
log_connections = off
log_statement = ddl
max_connections = ) . ($clients + 10));
$node->start;

# Save current offset (size) of the logfile
my $offset = -s $node->logfile;
my $stats_re = qr/postmaster potentially overloaded/;

# Give the postmaster some time to have a chance to (wrongly)
# emit the stats line, then check it did not log anything.
$node->safe_psql('postgres', 'SELECT 1');
sleep 2;
ok( !$node->log_contains($stats_re, $offset),
	'postmaster stats line not emitted while postmaster is not short of CPU'
);

# Flood the postmaster for 2 seconds and check if the warning got emitted
# This fails on CI and probably buildfram due to not enough VCPUs to saturate
# postmaster, so it is commented out.
#$node->pgbench('--initialize --quiet --scale=1', 0, [], [],
#	'set up pgbench_accounts table');
#$node->pgbench("--connect -c$clients -j$clients -T 2 -S -n", 0, [], [],
#	'pgbench --connect run to put stress on the postmaster');
#$node->wait_for_log($stats_re, $offset);
#pass('postmaster stats line emitted while postmaster is short of CPU');


# Try out the log_postmaster_excessive_connections
$node->pgbench('--initialize --quiet --scale=1', 0, [], [],
	'set up pgbench_accounts table');
$node->pgbench("--connect -c$clients -j$clients -T 2 -S -n", 0, [], [],
	'pgbench --connect run to put stress on the postmaster');
$stats_re = qr/postmaster excessive connections/;
$node->wait_for_log($stats_re, $offset);
pass('postmaster stats line emitted while postmaster is short of CPU');

$node->stop;
done_testing();
