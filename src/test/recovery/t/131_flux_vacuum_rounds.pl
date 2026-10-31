
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# VACUUM with more dead TIDs than maintenance_work_mem holds.  Heap's VACUUM
# stops its table scan when dead_items is full, vacuums the indexes and the
# table pages scanned so far, and scans on.  FLUX must do the same: neither
# stop early nor drop TIDs, and free a line pointer only in the round whose
# index vacuum removed its index entries.  Each case runs on a heap twin
# with the same expectations.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_vacuum_rounds');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\n" . "shared_preload_libraries = 'injection_points'\n");
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
CREATE EXTENSION pageinspect;
CREATE EXTENSION injection_points;
CREATE FUNCTION release_one(p text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE wait_event = p) THEN
	RETURN true;
  END IF;
  BEGIN
	PERFORM injection_points_wakeup(p);
  EXCEPTION WHEN OTHERS THEN
	NULL;
  END;
  RETURN false;
END $$;
});

# Line pointers on the table that are neither normal nor unused: what a
# VACUUM left behind for the next one.
my $nonunused = sub {
	my ($t) = @_;
	return $node->safe_psql(
		'postgres', qq{
SELECT count(*) FROM generate_series(0, pg_relation_size('$t') / 8192 - 1) b,
	heap_page_items(get_raw_page('$t', b::int)) WHERE lp_flags IN (2, 3)});
};

foreach my $am ('heap', 'flux')
{
	my $t = "vr_$am";

	# 60000 rows with two indexes, one row in two deleted: 30000 dead TIDs
	# take several times the 64 kB minimum maintenance_work_mem.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, v int) USING $am WITH (autovacuum_enabled = off);
INSERT INTO $t SELECT g, g FROM generate_series(1, 60000) g;
CREATE INDEX ${t}_id ON $t (id);
CREATE INDEX ${t}_v ON $t (v);
DELETE FROM $t WHERE id % 2 = 0;
});

	# VACUUM waits at each ambulkdelete: index_vacuum_count says which
	# round it is in.  In round 1 (the second), delete rows from the part
	# of the table the first round already compacted and from the part not
	# yet scanned.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('vacuum-bulkdel-one-index', 'wait')");
	my $vac = $node->background_psql('postgres');
	$vac->query_until(qr/start/,
		"\\echo start\nSET maintenance_work_mem = 64;\nVACUUM (PARALLEL 0) $t;\n"
	);

	# Each stop is at a distinct (round, index) pair, which tells it from
	# the stop just released while the wait event is still shown.
	$node->wait_for_event('client backend', 'vacuum-bulkdel-one-index');
	my $stops = 0;
	my $deleted_late = 0;
	my $max_bytes = 0;
	my $limit = 0;
	while (1)
	{
		my $prog = $node->safe_psql(
			'postgres', qq{
SELECT index_vacuum_count, indexes_processed, heap_blks_scanned,
	heap_blks_total, dead_tuple_bytes, max_dead_tuple_bytes
FROM pg_stat_progress_vacuum WHERE relid = '$t'::regclass});
		last if $prog eq '';
		my ($ivc, $done, $scanned, $total, $bytes, $max) = split /\|/, $prog;
		$max_bytes = $bytes if $bytes > $max_bytes;
		$limit = $max;

		if ($ivc == 1 && !$deleted_late)
		{
			# Rows 1 and 59999: one in a block already vacuumed, one
			# not yet scanned.  Neither is recorded in this round.
			$node->safe_psql('postgres',
				"DELETE FROM $t WHERE id IN (1, 59999)");
			$deleted_late = 1;
			cmp_ok($scanned, '<', $total,
				"$am: round 1 starts before the scan reaches the end");
		}
		$node->safe_psql('postgres',
			"SELECT injection_points_wakeup('vacuum-bulkdel-one-index')");
		die 'too many index vacuum stops' if ++$stops > 1000;

		# Next stop, or the end of the VACUUM.
		$node->poll_query_until(
			'postgres', qq{
SELECT NOT EXISTS (SELECT FROM pg_stat_progress_vacuum
				   WHERE relid = '$t'::regclass)
	OR EXISTS (SELECT FROM pg_stat_progress_vacuum p
			   JOIN pg_stat_activity a USING (pid)
			   WHERE p.relid = '$t'::regclass
				 AND a.wait_event = 'vacuum-bulkdel-one-index'
				 AND (p.index_vacuum_count, p.indexes_processed)
					 <> ($ivc, $done))}) or die 'VACUUM neither stopped nor finished';
	}
	note "$am: $stops index vacuum stops, dead items up to $max_bytes bytes";

	# A round ends once dead_items exceeds the limit, so it can overshoot
	# by one page's TIDs, but not grow with the number of dead rows.
	cmp_ok($max_bytes, '<=', 2 * $limit,
		"$am: dead_items stays within maintenance_work_mem");
	$vac->query_safe('SELECT 1');
	$vac->quit;
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('vacuum-bulkdel-one-index')");

	ok($deleted_late,
		"$am: VACUUM did more than one round of index vacuuming");

	# Every row deleted before VACUUM is gone from the table and from both
	# indexes.  The two late deletes may leave their TIDs dead (not
	# unused) for the next VACUUM, but never free a TID an index names.
	cmp_ok($nonunused->($t), '<=', 2,
		"$am: VACUUM freed every TID deleted before it started");
	is( $node->safe_psql(
			'postgres', qq{
SELECT count(*), sum(id) FROM $t}),
		'29998|899940000',
		"$am: rows after VACUUM");
	foreach my $idx ("${t}_id", "${t}_v")
	{
		is( $node->safe_psql(
				'postgres', "SELECT bt_index_check('$idx', true)"),
			'',
			"$am: bt_index_check heapallindexed on $idx");
	}

	# Reusing the freed TIDs must not resurrect index entries.
	$node->safe_psql('postgres',
		"INSERT INTO $t SELECT g, -g FROM generate_series(100001, 130000) g");
	is( $node->safe_psql(
			'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), sum(id) FROM $t WHERE id <= 60000}),
		'29998|899940000',
		"$am: index scan after TID reuse sees only the live rows");
	is( $node->safe_psql(
			'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM $t WHERE v < 0}),
		'30000',
		"$am: the new rows are reached through the second index");

	# The next VACUUM frees the late deletes too.  INDEX_CLEANUP ON keeps
	# heap from bypassing index vacuuming for so few dead items.
	$node->safe_psql('postgres', "VACUUM (INDEX_CLEANUP ON) $t");
	is($nonunused->($t), '0', "$am: a second VACUUM frees the rest");
	foreach my $idx ("${t}_id", "${t}_v")
	{
		is( $node->safe_psql(
				'postgres', "SELECT bt_index_check('$idx', true)"),
			'',
			"$am: bt_index_check heapallindexed on $idx after reuse");
	}
}

$node->stop;
done_testing();
