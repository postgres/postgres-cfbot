# Copyright (c) 2026, PostgreSQL Global Development Group
#
# The memory a statement holds for FLUX index scans and FLUX index
# maintenance must not grow with the number of rows.
#
# An index scan on a delete-marking (FLUX) table rechecks every entry it
# returns against the key of the version it serves
# (flux_entry_key_mismatch).  The recheck used to build the index's IndexInfo
# for every row, in the caller's memory context, and never free it: about 270
# bytes per row returned, more for an expression index, whose expression tree
# was copied too, all held to the end of the query.  A TPROC-H Q14 at SF100, a
# nested loop with an inner index scan, ran the server out of memory.  A
# uniqueness check that meets a delete-marked entry (table_index_fetch_tid),
# the in-place index maintenance of a key UPDATE, and its reversal by a
# writer that reverts an aborted UPDATE (flux_inplace_index_undo) built an
# IndexInfo per row the same way.
#
# Each statement reads its own executor's memory (the outermost ExecutorState
# context and everything under it) once the work under test has run to
# completion, for n and 2n rows: it must stay below a small bound and must not
# grow with n.  The heap twin of every table is the control.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $injection = $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('flux_recheck_memory');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_wal_size = 4GB
undo_rollback_overflow_size = 0
));
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'")
  if $injection;
$node->start;
$injection &&= $node->check_extension('injection_points');

my $n1 = 1_000_000;    # rows returned by a scan
my $m1 = 100_000;      # rows changed by a DML statement
my $bound = 8 * 1024 * 1024;
my $growth = 1024 * 1024;

# A plan that runs the work under test the same way for both access methods
# and holds no rows in memory beyond work_mem.
my $settings = q{
SET max_parallel_workers_per_gather = 0;
SET work_mem = '64kB';
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_memoize = off;
SET enable_material = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
};

$node->safe_psql(
	'postgres', q{
CREATE VIEW exec_bytes AS
  SELECT sum(c.total_bytes)::bigint AS bytes
  FROM pg_backend_memory_contexts c,
       (SELECT path, level FROM pg_backend_memory_contexts
        WHERE name = 'ExecutorState' ORDER BY level LIMIT 1) t
  WHERE t.path[t.level] = ANY (c.path);
});

# Run $work, a query that returns the number n of rows it handled, as a
# materialized CTE (after the data-modifying CTEs in $with, if any), and read
# the executor's memory in the same statement once n is known.  Returns n and
# the bytes.
sub measure
{
	my ($set, $with, $work) = @_;
	my $res = $node->safe_psql(
		'postgres', qq{$settings $set
WITH $with r AS MATERIALIZED ($work)
SELECT n || ',' || (SELECT bytes FROM exec_bytes WHERE n IS NOT NULL) FROM r;
});
	return split /,/, $res;
}

# $sql with T replaced by the table name and the bounds by their values.
sub fill
{
	my ($sql, $t, %v) = @_;
	$sql =~ s/\bT(?=\b|_)/$t/g;
	$sql =~ s/\b$_\b/$v{$_}/g foreach keys %v;
	return $sql;
}

# Check one case for n and 2n rows: the right count, bounded memory, and
# no growth with the rows.
sub check_case
{
	my ($name, $set, $with, $work, $t, $n, $setup) = @_;
	my %b;
	foreach my $round (1, 2)
	{
		my $lo = $round == 1 ? 0 : $n;
		my $hi = $round == 1 ? $n : 3 * $n;
		my %v = (LO => $lo, HI => $hi);
		$setup->($lo, $hi) if $setup;
		my ($rows, $bytes) =
		  measure($set, fill($with, $t, %v), fill($work, $t, %v));
		is($rows, $hi - $lo, "$name, $t, " . ($hi - $lo) . " rows: count");
		cmp_ok($bytes, '<', $bound,
			"$name, $t, " . ($hi - $lo) . " rows: executor memory bounded");
		note "$name, $t, " . ($hi - $lo) . " rows: $bytes bytes";
		$b{$round} = $bytes;
	}
	cmp_ok($b{2} - $b{1},
		'<', $growth,
		"$name, $t: executor memory does not grow with the rows");
}

# Index scans: each returns HI - LO rows through a btree index on a 2n-row
# table, with every entry rechecked on FLUX.
foreach my $am ('flux', 'heap')
{
	my $t = "s_$am";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (k int PRIMARY KEY, v int) USING $am;
INSERT INTO $t SELECT g, g FROM generate_series(1, 3 * $n1) g;
CREATE INDEX ${t}_expr ON $t ((v * 2));
ANALYZE $t;
});
}
my @scans = (
	[
		'nested loop',
		'Nested Loop.*Index Scan using T_pkey',
		'SELECT count(p.v) AS n FROM generate_series(LO + 1, HI) g JOIN T p ON p.k = g'
	],
	[
		'index scan',
		'Index Scan using T_pkey',
		'SELECT count(v) AS n FROM T WHERE k > LO AND k <= HI'
	],
	[
		'expression index scan',
		'Index Scan using T_expr',
		'SELECT count(k) AS n FROM T WHERE v * 2 > 2 * LO AND v * 2 <= 2 * HI'
	],);
foreach my $c (@scans)
{
	my ($name, $plan, $work) = @$c;
	foreach my $am ('flux', 'heap')
	{
		my $t = "s_$am";
		like(
			$node->safe_psql(
				'postgres',
				"$settings SET enable_seqscan = off; EXPLAIN (COSTS OFF) "
				  . fill($work, $t, LO => 0, HI => $n1)),
			qr/${\ fill($plan, $t)}/s,
			"$name, $t: plan");
		check_case($name, 'SET enable_seqscan = off;', '', $work, $t, $n1);
	}
}

# In-place key UPDATEs (flux_index_change_exact for the partial index,
# flux_inplace_index_maintenance for both), then INSERTs of the old keys:
# each meets the delete-marked entry of its key in the primary key, which the
# uniqueness check judges through table_index_fetch_tid.
my $noidx = 'SET enable_indexscan = off;';
foreach my $am ('flux', 'heap')
{
	my $t = "u_$am";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (k int PRIMARY KEY, v int) USING $am;
CREATE INDEX ${t}_part ON $t (k) WHERE v >= 0;
INSERT INTO $t SELECT g, g FROM generate_series(1, 3 * $m1) g;
ANALYZE $t;
});
	check_case(
		'key update',
		$noidx,
		'u AS (UPDATE T SET k = k + 10000000 WHERE k > LO AND k <= HI RETURNING 1),',
		'SELECT count(*) AS n FROM u',
		$t,
		$m1);
	check_case(
		'unique insert of a delete-marked key',
		$noidx,
		'i AS (INSERT INTO T SELECT g, g FROM generate_series(LO + 1, HI) g RETURNING 1),',
		'SELECT count(*) AS n FROM i',
		$t,
		$m1);
}

# A writer that meets an aborted key UPDATE whose rollback is still pending
# reverts it first, index changes included (flux_inplace_index_undo).  Hold
# the undo apply workers so that every rollback stays pending.
if ($injection)
{
	$node->safe_psql(
		'postgres', q{
CREATE EXTENSION injection_points;
SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');
-- Wake one held undo apply worker; true once none is held any more.
CREATE FUNCTION release_one() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
                 WHERE wait_event = 'pbu-undo-worker-before-apply') THEN
    RETURN true;
  END IF;
  BEGIN
    PERFORM injection_points_wakeup('pbu-undo-worker-before-apply');
  EXCEPTION WHEN OTHERS THEN
    NULL;  -- it left the injection point meanwhile
  END;
  RETURN false;
END $$;
});
	my $held = 0;
	foreach my $am ('flux', 'heap')
	{
		my $t = "r_$am";
		$node->safe_psql(
			'postgres', qq{
CREATE TABLE $t (k int PRIMARY KEY, v int) USING $am;
INSERT INTO $t SELECT g, g FROM generate_series(1, 3 * $m1) g;
ANALYZE $t;
});
		check_case(
			'revert of an aborted key update',
			$noidx,
			'u AS (UPDATE T SET v = v + 1 WHERE k > LO AND k <= HI RETURNING 1),',
			'SELECT count(*) AS n FROM u',
			$t, $m1,
			sub {
				my ($lo, $hi) = @_;
				$node->safe_psql('postgres',
					"BEGIN; UPDATE $t SET k = k + 10000000 WHERE k > $lo AND k <= $hi; ROLLBACK;"
				);
				# An undo apply worker holds the rollback, one per rollback.
				return unless $am eq 'flux';
				$held++;
				ok( $node->poll_query_until(
						'postgres', qq{
SELECT count(*) >= $held FROM pg_stat_activity
 WHERE backend_type = 'undo apply worker'
   AND wait_event = 'pbu-undo-worker-before-apply'}),
					"$t: the rollback is pending");
			});
	}
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
	ok($node->poll_query_until('postgres', 'SELECT release_one()'),
		'held undo apply workers released');

	# The discard worker discards an aborted transaction's per-backend undo
	# only once its rollback is applied.  pg_stat_get_undo_retention()
	# reports what holds back the cluster-wide undo in the WAL, not these
	# rollbacks.
	ok( $node->poll_query_until(
			'postgres', q{
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT coalesce(bool_and(discard = insert), true)
        FROM pg_stat_get_undo_logs() WHERE persistence = 'permanent')}),
		'every rollback applied');
}

# The FLUX tables hold what their heap twins hold, through a sequential scan
# and through every index, and every FLUX btree index is intact.
my $have_amcheck = $node->check_extension('amcheck');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck') if $have_amcheck;
foreach my $p ('s', 'u', ($injection ? ('r') : ()))
{
	my $q =
	  "SELECT count(*) || ':' || sum(k::bigint) || ':' || sum(v::bigint) FROM T";
	my @res;
	foreach my $am ('flux', 'heap')
	{
		my $t = "${p}_$am";
		push @res,
		  $node->safe_psql('postgres',
				"SET enable_indexscan = off; SET enable_bitmapscan = off; "
			  . fill($q, $t) . ";\n"
			  . "SET enable_seqscan = off; SET enable_indexscan = on; "
			  . fill($q, $t)
			  . " WHERE k > 0;");
	}
	is($res[0], $res[1], "${p}_flux: rows as heap");
	next unless $have_amcheck;
	foreach my $i (
		split /\n/,
		$node->safe_psql(
			'postgres',
			"SELECT indexrelid::regclass FROM pg_index WHERE indrelid = '${p}_flux'::regclass"
		))
	{
		my ($out, $err);
		my $rc = $node->psql(
			'postgres',
			"SELECT bt_index_check('$i'::regclass, heapallindexed => true)",
			stdout => \$out,
			stderr => \$err);
		is("$rc:$err", '0:', "$i: bt_index_check");
	}
}

unlike(
	slurp_file($node->logfile),
	qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
	'no failed assertion, mismatched or abandoned undo');
$node->stop;
done_testing();
