# Copyright (c) 2026, PostgreSQL Global Development Group

# Run the core regression tests while replacing supported DDL commands with
# their deparsed reconstruction (see test_ddl_deparse.execute_deparsed_ddl).
# This is meant to reliably detect deparsing support gaps for new syntax:
# any divergence between a command and its deparsed form shows up as a
# regression diff.
use strict;
use warnings FATAL => 'all';

use Cwd            qw(abs_path);
use File::Basename qw(dirname);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Tests that are incompatible with DDL replacement can be excluded here,
# with a comment explaining why.
my @excluded_tests = (
	# This test lists every event trigger in the database, and the capture
	# trigger the test_ddl_deparse extension installs shows up there.  It
	# also installs sql_drop/ddl_command_end event triggers whose functions
	# reference objects by unqualified name, which fail when the replayed
	# DDL -- executed with search_path cleared, as an apply worker would --
	# fires them.
	'event_trigger',
);

# Initialize the primary node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init();

$node->append_conf('postgresql.conf', <<EOM);
shared_preload_libraries = 'test_ddl_deparse'
test_ddl_deparse.execute_deparsed_ddl = on
test_ddl_deparse.log_declined_commands = on
wal_level = replica
EOM
$node->start;

my $srcdir = abs_path("../../../..");

# --dlpath is needed to be able to find the location of regress.so
# and any libraries the regression tests require.
my $dlpath = dirname($ENV{REGRESS_SHLIB});

# --outputdir points to the path where to place the output files.
my $outputdir = $PostgreSQL::Test::Utils::tmp_check;

# --inputdir points to the path of the input files.
my $inputdir = "$srcdir/src/test/regress";

# Build the schedule, dropping any excluded tests.
my $schedule = "$outputdir/deparse_schedule";
open my $in, '<', "$inputdir/parallel_schedule"
  or die "could not open parallel_schedule: $!";
open my $out, '>', $schedule
  or die "could not create $schedule: $!";
while (my $line = <$in>)
{
	if ($line =~ /^test:\s+(.*)$/)
	{
		my @tests = grep {
			my $t = $_;
			!grep { $t eq $_ } @excluded_tests
		} split(/\s+/, $1);
		next unless @tests;
		$line = "test: " . join(' ', @tests) . "\n";
	}
	print $out $line;
}
close $in;
close $out;

# Run the tests.
my $rc =
  system($ENV{PG_REGRESS} . " "
	  . "--bindir= "
	  . "--dlpath=\"$dlpath\" "
	  . "--load-extension=test_ddl_deparse "
	  . "--host="
	  . $node->host . " "
	  . "--port="
	  . $node->port . " "
	  . "--schedule=$schedule "
	  . "--max-concurrent-tests=20 "
	  . "--inputdir=\"$inputdir\" "
	  . "--expecteddir=\"$inputdir\" "
	  . "--outputdir=\"$outputdir\"");

# Dump out the regression diffs file, if there is one
if ($rc != 0)
{
	my $diffs = "$outputdir/regression.diffs";
	if (-e $diffs)
	{
		print "=== dumping $diffs ===\n";
		print slurp_file($diffs);
		print "=== EOF ===\n";
	}
}

# Report the commands the deparser declined, grouped by command tag.  These
# executed unchanged and were therefore not exercised by this run; the list is
# how a deparser gap becomes visible instead of just being absent from the
# coverage.  It is printed rather than asserted on, since the counts move with
# every change to the core tests, and since a command being declined is not by
# itself a defect.
my %declined;
for my $line (split /\n/, slurp_file($node->logfile))
{
	next unless $line =~ /test_ddl_deparse: declined ([A-Z][A-Z ]*[A-Z]):/;
	$declined{$1}++;
}
if (%declined)
{
	note("commands declined by the deparser during this run:");
	note(sprintf("  %-40s %d", $_, $declined{$_}))
	  for sort { $declined{$b} <=> $declined{$a} || $a cmp $b } keys %declined;
}
else
{
	note("the deparser declined no command during this run");
}

# Report results
is($rc, 0, 'regression tests pass');

done_testing();
