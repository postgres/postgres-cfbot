
# Copyright (c) 2026, PostgreSQL Global Development Group

# With retain_dead_tuples, a replicated UPDATE of a row that was deleted
# locally must be reported as an update_deleted conflict, naming the local
# deleting transaction, as for heap; not as update_missing.  The apply worker
# finds the deleted version with a SnapshotAny scan, through the replica
# identity index or, without a usable one, sequentially.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $am ('heap', 'flux')
{
	my $pub = PostgreSQL::Test::Cluster->new("pub_$am");
	$pub->init(allows_streaming => 'logical');
	$pub->start;

	# retain_dead_tuples needs a replication slot and commit timestamps on
	# the subscriber.  No autovacuum, so that only the test's own VACUUM runs.
	my $sub = PostgreSQL::Test::Cluster->new("sub_$am");
	$sub->init(allows_streaming => 'logical');
	$sub->append_conf(
		'postgresql.conf', qq{
track_commit_timestamp = on
autovacuum = off
});
	$sub->start;

	my $ddl = qq{
		CREATE TABLE pk (a int PRIMARY KEY, b int) USING $am;
		CREATE TABLE full_ri (a int, b int) USING $am;
		ALTER TABLE full_ri REPLICA IDENTITY FULL;
	};
	$pub->safe_psql('postgres', $ddl);
	$sub->safe_psql('postgres', $ddl);
	$pub->safe_psql('postgres', 'CREATE PUBLICATION p FOR TABLE pk, full_ri');
	my $connstr = $pub->connstr . ' dbname=postgres';
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p WITH (retain_dead_tuples = true)"
	);
	$sub->wait_for_subscription_sync($pub, 's');
	ok( $sub->poll_query_until(
			'postgres',
			"SELECT xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
		),
		"$am: conflict detection slot holds an xmin");

	$pub->safe_psql('postgres',
		'INSERT INTO pk VALUES (1, 1), (2, 2); INSERT INTO full_ri VALUES (1, 1), (2, 2)'
	);
	$pub->wait_for_catchup('s');

	# Stop the apply worker, so that the remote UPDATEs arrive after the
	# local DELETEs.  The slot's xmin cannot advance meanwhile.
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s DISABLE');
	$sub->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
	) or die "timed out waiting for the apply worker to stop";

	$pub->safe_psql('postgres',
		'UPDATE pk SET b = 10 WHERE a = 1; UPDATE full_ri SET b = 10 WHERE a = 1'
	);
	$sub->safe_psql('postgres',
		'DELETE FROM pk WHERE a = 1; DELETE FROM full_ri WHERE a = 1');

	# The deleted rows are still needed for conflict detection: VACUUM keeps
	# them, and says so.
	for my $tab ('pk', 'full_ri')
	{
		my ($ret, $stdout, $stderr) =
		  $sub->psql('postgres', "VACUUM (VERBOSE) $tab");
		like(
			$stderr,
			qr/1 are dead but not yet removable/,
			"$am: VACUUM keeps the deleted row of $tab");
	}

	my $offset = -s $sub->logfile;
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s ENABLE');
	$pub->wait_for_catchup('s');
	my $log = slurp_file($sub->logfile, $offset);

	# Primary key replica identity: the deleted row is found by index.
	like(
		$log,
		qr/conflict detected on relation "public.pk": conflict=update_deleted.*
.*DETAIL:.* Could not find the row to be updated: remote row \(1, 10\), replica identity \(a\)=\(1\).
.*The row to be updated was deleted locally in transaction [0-9]+ at .*/,
		"$am: update_deleted detected through the replica identity index");

	# REPLICA IDENTITY FULL without an index: found by a sequential scan.
	like(
		$log,
		qr/conflict detected on relation "public.full_ri": conflict=update_deleted.*
.*DETAIL:.* Could not find the row to be updated: remote row \(1, 10\), replica identity full \(1, 1\).*
.*The row to be updated was deleted locally in transaction [0-9]+ at .*/,
		"$am: update_deleted detected by a sequential scan");

	unlike($log, qr/conflict=update_missing/,
		"$am: no update_missing reported for a locally deleted row");

	is( $sub->safe_psql(
			'postgres',
			'SELECT (SELECT count(*) FROM pk) || \',\' || (SELECT count(*) FROM full_ri)'
		),
		'1,1',
		"$am: the remote updates were skipped");

	$sub->stop;
	$pub->stop;
}

done_testing();
