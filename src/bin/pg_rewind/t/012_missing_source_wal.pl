# Copyright (c) 1996-2026, PostgreSQL Global Development Group
#
# Test pg_rewind behavior when the source server has recycled WAL segments
# needed for recovery.
#
# Two sub-tests:
#
# 1. Without WAL archiving: pg_rewind succeeds but the rewound server
#    must refuse to start because minRecoveryPoint cannot be reached
#    (the first TL2 WAL segment at the divergence point is missing).
#
# 2. With WAL archiving: same scenario, but restore_command is configured
#    so recovery can fetch the missing segment from the archive. The
#    rewound server should start and operate normally.

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#
# Helper: set up the common scenario (primary + standby, promote, diverge,
# VACUUM FULL to recycle WAL, run pg_rewind).
#
# Returns ($node_primary, $node_standby).
#
sub setup_rewind_with_recycled_wal
{
	my (%opts) = @_;
	my $test_name = $opts{test_name} || 'test';
	my $archive = $opts{archive} || 0;

	my $node_primary =
	  PostgreSQL::Test::Cluster->new("primary_$test_name");
	$node_primary->init(allows_streaming => 1);
	$node_primary->append_conf(
		'postgresql.conf', qq(
wal_keep_size = 320MB
wal_log_hints = on
));
	if ($archive)
	{
		$node_primary->enable_archiving();
	}
	$node_primary->start;
	$node_primary->safe_psql('postgres', 'CREATE DATABASE testdb');

	$node_primary->backup('my_backup');
	my $node_standby =
	  PostgreSQL::Test::Cluster->new("standby_$test_name");
	$node_standby->init_from_backup($node_primary, 'my_backup',
		has_streaming => 1);
	# Aggressive WAL recycling on the standby
	$node_standby->append_conf(
		'postgresql.conf', qq(
wal_keep_size = 0
min_wal_size = 32MB
max_wal_size = 48MB
));
	if ($archive)
	{
		$node_standby->enable_archiving();
	}
	$node_standby->start;

	$node_primary->wait_for_catchup($node_standby, 'write');
	$node_standby->promote;
	$node_standby->poll_query_until('testdb',
		"SELECT NOT pg_is_in_recovery()");

	# Parse divergence LSN from the timeline history file
	my $standby_pgdata = $node_standby->data_dir;
	my $history_content =
	  slurp_file("$standby_pgdata/pg_wal/00000002.history");
	my ($diverge_lsn) = ($history_content =~ /^1\t([0-9A-F\/]+)\t/m);
	die "could not parse timeline history" unless $diverge_lsn;

	my $diverge_seg = $node_standby->safe_psql('postgres',
		"SELECT pg_walfile_name('$diverge_lsn'::pg_lsn)");
	my $tl2_diverge_seg = $diverge_seg;
	$tl2_diverge_seg =~ s/^0000000./00000002/;
	note "divergence LSN: $diverge_lsn, TL2 segment: $tl2_diverge_seg";

	$node_primary->stop('fast');

	# Save primary's postgresql.conf
	my $primary_pgdata = $node_primary->data_dir;
	my $tmp_folder = PostgreSQL::Test::Utils::tempdir;
	copy("$primary_pgdata/postgresql.conf",
		"$tmp_folder/primary-postgresql.conf.tmp")
	  or die "copy failed: $!";

	# Generate WAL on promoted standby to force recycling
	$node_standby->safe_psql('testdb',
		"CREATE TABLE t1 (id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY, f1 TEXT)"
	);
	$node_standby->safe_psql('testdb',
		"INSERT INTO t1 (f1) SELECT repeat('ghijk', 100) FROM generate_series(1, 200000)"
	);
	$node_standby->safe_psql('testdb', "CHECKPOINT");
	$node_standby->safe_psql('testdb', "VACUUM FULL");

	# Verify TL2 segment at divergence point was recycled
	ok(!-e "$standby_pgdata/pg_wal/$tl2_diverge_seg",
		"$test_name: source recycled TL2 WAL segment $tl2_diverge_seg");

	$node_standby->stop('fast');

	# Run pg_rewind
	my ($stdout, $stderr) = run_command(
		[
			'pg_rewind',     '--debug',
			'--source-pgdata' => $standby_pgdata,
			'--target-pgdata' => $primary_pgdata,
			'--no-sync',
			'--config-file'  => "$tmp_folder/primary-postgresql.conf.tmp",
		]);
	ok($? == 0, "$test_name: pg_rewind succeeds");

	# Restore primary's postgresql.conf
	copy("$tmp_folder/primary-postgresql.conf.tmp",
		"$primary_pgdata/postgresql.conf")
	  or die "copy failed: $!";

	return ($node_primary, $node_standby);
}

###########################################################################
# Test 1: Without archive - server must refuse to start
###########################################################################

my ($pri1, $stby1) = setup_rewind_with_recycled_wal(
	test_name => 'noarchive');

# The server should fail to start because minRecoveryPoint is unreachable
my $ret = $pri1->start(fail_ok => 1);
ok($ret == 0,
	"noarchive: rewound server fails to start with missing WAL");

# Check that the log contains the expected FATAL about consistency
my $logfile = slurp_file($pri1->logfile);
like($logfile, qr/FATAL:.*WAL ends before consistent recovery point/,
	"noarchive: log reports WAL ends before consistent recovery point");

$pri1->teardown_node;
$stby1->teardown_node;

###########################################################################
# Test 2: With archive - recovery fetches missing WAL, server starts fine
###########################################################################

my ($pri2, $stby2) = setup_rewind_with_recycled_wal(
	test_name => 'archive',
	archive   => 1);

# Set up restore_command so recovery can fetch from the archive.
# Use the standby's archive directory since that's where the TL2 WAL
# was archived before recycling.
$pri2->enable_restoring($stby2, 0);

# Also need recovery.signal for archive recovery
$pri2->set_standby_mode();

$pri2->start;

# Verify we can connect and see the data
my ($psql_ret, $psql_stdout, $psql_stderr) = $pri2->psql(
	'testdb', "SELECT count(*) FROM t1");
is($psql_ret, 0, "archive: connecting to testdb succeeds after rewind");
is($psql_stdout, '200000',
	"archive: all rows visible after rewind with archived WAL");

$pri2->teardown_node;
$stby2->teardown_node;

done_testing();
