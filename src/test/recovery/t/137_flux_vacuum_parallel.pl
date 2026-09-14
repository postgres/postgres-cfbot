
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX VACUUM vacuums its indexes with parallel workers, as heap's does.
# Parallel and serial VACUUM of the same contents must leave the same
# result: rows, index contents (amcheck heapallindexed, index-only counts
# per index), pg_stat and pg_class counters.  The index work includes what
# only FLUX has: delete-marked entries of in-place key UPDATEs, removed in a
# worker through table_index_tombstone_removable, and rows of an aborted
# key UPDATE whose revert is still pending.  Also: several rounds under a
# small maintenance_work_mem, a crash while the workers run, and a standby.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_vacuum_parallel');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
shared_preload_libraries = 'injection_points'
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 4
min_parallel_index_scan_size = 0
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
CREATE EXTENSION injection_points;
});

# A slot keeps the WAL the standby has not received yet: with
# wal_consistency_checking the tables below write far more than max_wal_size.
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('standby_slot')");
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('flux_vacuum_parallel_standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"primary_slot_name = 'standby_slot'");
$standby->start;

# Two tables of identical contents: four btree indexes, a hash, a brin and a
# partial btree; brin's cleanup, run by a worker, scans the table.  In-place key UPDATEs of a and b leave delete-marked
# entries; DELETEs leave dead TIDs; a rolled-back key UPDATE leaves rows
# whose versions are an aborted writer's.
sub make_table
{
	my ($t, $am) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, a int, b int, c text, d int) USING $am
	WITH (autovacuum_enabled = off);
INSERT INTO $t SELECT g, g % 997, g % 1009, 'c' || g, -g
	FROM generate_series(1, 40000) g;
CREATE INDEX ${t}_id ON $t (id);
CREATE INDEX ${t}_a ON $t (a);
CREATE INDEX ${t}_b ON $t (b);
CREATE INDEX ${t}_ab ON $t (a, b);
CREATE INDEX ${t}_c ON $t USING hash (c);
CREATE INDEX ${t}_d ON $t USING brin (d) WITH (pages_per_range = 4);
CREATE INDEX ${t}_p ON $t (id) WHERE id % 10 = 3;
-- block ranges brin has not summarized, for its cleanup to summarize
INSERT INTO $t SELECT g, g % 997, g % 1009, 'c' || g, -g
	FROM generate_series(40001, 44000) g;
UPDATE $t SET a = a + 5000 WHERE id % 7 = 0;
UPDATE $t SET b = b + 7000 WHERE id % 11 = 0;
DELETE FROM $t WHERE id % 5 = 0;
BEGIN;
UPDATE $t SET a = a + 9000 WHERE id % 13 = 0;
ROLLBACK;
});
}

my @idx = qw(id a b ab p);

# Everything parallel and serial VACUUM must agree on.
sub state_of
{
	my ($t) = @_;
	my $s = $node->safe_psql(
		'postgres', qq{
SELECT count(*), sum(id), sum(a), sum(b), sum(length(c)), sum(d) FROM $t});
	foreach my $i (@idx)
	{
		$s .= "\n$i: "
		  . $node->safe_psql('postgres',
			"SELECT bt_index_check('${t}_$i', true)");
	}
	# Every index reaches every row, by its own key.
	$s .= "\n" . $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = on;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SELECT (SELECT count(*) FROM $t WHERE id > 0),
	(SELECT count(*) FROM $t WHERE a >= 0),
	(SELECT count(*) FROM $t WHERE b >= 0),
	(SELECT count(*) FROM $t WHERE a >= 0 AND b >= 0),
	(SELECT count(*) FROM $t WHERE c = 'c4001'),
	(SELECT count(*) FROM $t WHERE d < 0),
	(SELECT count(*) FROM $t WHERE id % 10 = 3 AND id > 0)});
	$s .= "\n" . $node->safe_psql(
		'postgres', qq{
SELECT n_live_tup, n_dead_tup, vacuum_count FROM pg_stat_user_tables
WHERE relid = '$t'::regclass});
	$s .= "\n" . $node->safe_psql(
		'postgres', qq{
SELECT string_agg(substr(c.relname, length('$t') + 2) || '=' ||
				  c.relpages || '/' || c.reltuples, ' ' ORDER BY c.relname)
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
WHERE i.indrelid = '$t'::regclass});
	$s .= "\n"
	  . $node->safe_psql('postgres',
		"SELECT relpages, reltuples FROM pg_class WHERE oid = '$t'::regclass"
	  );
	return $s;
}

# VACUUM VERBOSE; return the number of workers it launched in total.
sub vacuum_launched
{
	my ($opts, $t, $mwm) = @_;
	my ($stdout, $stderr);
	$node->psql(
		'postgres', "SET maintenance_work_mem = '$mwm';
		 SET client_min_messages = info; VACUUM (VERBOSE, $opts) $t;
		 SELECT pg_stat_force_next_flush();",
		stdout => \$stdout,
		stderr => \$stderr,
		on_error_die => 1);
	my $n = 0;
	$n += $1 while $stderr =~ /launched (\d+) parallel vacuum worker/g;
	return $n;
}

foreach my $am ('heap', 'flux')
{
	foreach my $mwm ('64MB', '64kB')
	{
		make_table("vs_$am", $am);
		make_table("vp_$am", $am);
		my $before = state_of("vs_$am");
		is(state_of("vp_$am"), $before, "$am $mwm: twins start equal");

		is(vacuum_launched('PARALLEL 0', "vs_$am", $mwm),
			0, "$am $mwm: PARALLEL 0 launches no worker");
		cmp_ok(vacuum_launched('PARALLEL 4', "vp_$am", $mwm),
			'>', 0, "$am $mwm: PARALLEL 4 launches workers");

		my $serial = state_of("vs_$am");
		is(state_of("vp_$am"), $serial,
			"$am $mwm: parallel and serial VACUUM leave the same state");
		note "$am $mwm serial state:\n$serial";

		# Delete-marked entries left by the UPDATEs are gone in both.
		my $tomb = sub {
			my ($t) = @_;
			return $node->safe_psql(
				'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = on;
SELECT count(*) FROM $t WHERE a >= 5000 AND a < 9000});
		};
		is($tomb->("vp_$am"), $tomb->("vs_$am"),
			"$am $mwm: same index-only count on the updated key range");

		# A second VACUUM of each finds nothing more to do, in the same way.
		vacuum_launched('PARALLEL 0', "vs_$am", $mwm);
		vacuum_launched('PARALLEL 4', "vp_$am", $mwm);
		is(state_of("vp_$am"), state_of("vs_$am"),
			"$am $mwm: and after a second VACUUM");

		$node->safe_psql('postgres', "DROP TABLE vs_$am, vp_$am");
	}
}

# VACUUM (PARALLEL n) of a temporary FLUX table: as heap, a WARNING.
{
	my ($stdout, $stderr);
	$node->psql(
		'postgres', q{
CREATE TEMP TABLE tf (a int) USING flux;
CREATE INDEX ON tf (a); CREATE INDEX ON tf ((a + 1));
INSERT INTO tf SELECT generate_series(1, 100);
VACUUM (PARALLEL 2) tf;},
		stdout => \$stdout,
		stderr => \$stderr);
	like(
		$stderr,
		qr/disabling parallel option of vacuum on "tf" --- cannot vacuum temporary tables in parallel/,
		'temporary FLUX table: parallel VACUUM disabled with a WARNING');
}

# Crash while parallel workers vacuum the indexes: the next VACUUM finishes
# the job and the indexes are sound.
make_table('vc', 'flux');
my $expect = $node->safe_psql('postgres',
	'SELECT count(*), sum(id), sum(a), sum(b) FROM vc');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('vacuum-bulkdel-one-index', 'wait')");
my $vac = $node->background_psql('postgres', on_error_stop => 0);
$vac->query_until(qr/start/, "\\echo start\nVACUUM (PARALLEL 4) vc;\n");
$node->poll_query_until(
	'postgres', q{
SELECT count(*) >= 2 FROM pg_stat_activity
WHERE wait_event = 'vacuum-bulkdel-one-index'}
) or die 'parallel VACUUM did not reach the index phase';
is( $node->safe_psql(
		'postgres', q{
SELECT count(*) > 0 FROM pg_stat_activity
WHERE backend_type = 'parallel worker'
  AND wait_event = 'vacuum-bulkdel-one-index'}),
	't',
	'a parallel worker is vacuuming an index');
$node->stop('immediate');
eval { $vac->quit };
$node->start;
is( $node->safe_psql(
		'postgres', 'SELECT count(*), sum(id), sum(a), sum(b) FROM vc'),
	$expect,
	'rows survive a crash during parallel index vacuum');
foreach my $i (@idx)
{
	is( $node->safe_psql('postgres', "SELECT bt_index_check('vc_$i', true)"),
		'',
		"after crash: bt_index_check heapallindexed on vc_$i");
}
cmp_ok(vacuum_launched('PARALLEL 4', 'vc', '64MB'),
	'>', 0, 'parallel VACUUM after the crash');
foreach my $i (@idx)
{
	is( $node->safe_psql('postgres', "SELECT bt_index_check('vc_$i', true)"),
		'',
		"after VACUUM: bt_index_check heapallindexed on vc_$i");
}

# The standby replays all of it.
$node->wait_for_catchup($standby);
is( $standby->safe_psql(
		'postgres', 'SELECT count(*), sum(id), sum(a), sum(b) FROM vc'),
	$expect,
	'standby: same rows');
is( $standby->safe_psql(
		'postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM vc WHERE a >= 0}),
	$node->safe_psql(
		'postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM vc WHERE a >= 0}),
	'standby: index scan agrees with the primary');
foreach my $i (@idx)
{
	is( $standby->safe_psql(
			'postgres', "SELECT bt_index_check('vc_$i', true)"),
		'',
		"standby: bt_index_check heapallindexed on vc_$i");
}

$standby->stop;
$node->stop;
done_testing();
