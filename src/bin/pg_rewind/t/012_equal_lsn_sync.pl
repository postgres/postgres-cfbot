# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize and start the primary node (Node A)
my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1);
$node_a->start;

# Initialize the standby node (Node B) from a backup of Node A
my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_a->backup('my_backup');
$node_b->init_from_backup($node_a, 'my_backup', has_streaming => 1);
$node_b->start;

# Wait for the standby to catch up to ensure WAL is identical
$node_a->wait_for_catchup($node_b, 'replay', $node_a->lsn('insert'));

# stop NODE A cleanly. 
# This is the critical step to trigger the bug. A clean shutdown writes a 
# shutdown checkpoint. Node A's WAL now ends exactly at this LSN.
$node_a->stop;

# Promote Node B. 
# It writes an end-of-recovery record at the exact LSN where Node A stopped.
# divergerec will now perfectly equal target_wal_endrec.
$node_b->promote;

# Make a non-WAL-logged change on the new primary (Node B).
# ALTER SYSTEM modifies postgresql.auto.conf but generates no WAL.
$node_b->safe_psql('postgres', "ALTER SYSTEM SET work_mem = '50MB';");

# Run pg_rewind using run_command to capture all output
my ($stdout, $stderr) = run_command(
	[
		'pg_rewind', '--debug',
		'--source-server' => $node_b->connstr,
		'--target-pgdata' => $node_a->data_dir,
		'--no-sync'
	]
);

my ($divergerec) = $stderr =~ /servers diverged at WAL location ([A-F0-9X\/]+) on timeline/;
my ($target_wal_end) = $stderr =~ /target WAL ends at ([A-F0-9X\/]+)/;

ok(defined $divergerec, "Found divergence LSN in logs: $divergerec");
ok(defined $target_wal_end, "Found target WAL end LSN in logs: $target_wal_end");

# It asserts that the test successfully triggered the exact boundary condition.
is(
	$target_wal_end, 
	$divergerec, 
	'Target WAL ends exactly at the divergence point (0 WAL changes)'
);

# If pg_rewind wrongly assumes it can skip the sync, it prints this exact line.
# We want to ensure it DOES NOT print this line.
unlike(
	$stderr,
	qr/no rewind required/,
	'pg_rewind correctly recognized that differing timelines require a rewind'
);

# Verify the file sync actually occurred
my $auto_conf_target = slurp_file($node_a->data_dir . '/postgresql.auto.conf');
like(
	$auto_conf_target,
	qr/work_mem = '50MB'/,
	'postgresql.auto.conf was synchronized from the source even WAL LSNs matched'
);

done_testing();