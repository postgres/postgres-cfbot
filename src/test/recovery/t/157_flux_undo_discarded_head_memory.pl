# Copyright (c) 2026, PostgreSQL Global Development Group
#
# An UPDATE of FLUX rows whose newest version's undo record has been
# discarded must not hold memory per row.
#
# Before FLUX updates a row in place it reads the undo record of the row's
# newest version (FluxTupleHasCommittedUpdateAfter, through FluxPbuFetchXid
# and UndoFetchRecord) to find out who wrote it.  Once that undo has been
# discarded, UndoFetchRecord returned NULL without freeing the record it had
# allocated: about 170 bytes of the statement's memory per row updated, and
# twice that for an UPDATE that changes an index key.
#
# Every row is updated once and its undo discarded; then an UPDATE of n rows
# and one of 2n rows read the memory of their executor (the outermost
# ExecutorState context and everything under it) in the same statement, once
# they are done.  It must stay below a small bound and must not grow with n.
# The heap twin of every table is the control.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_undo_discarded_head');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_wal_size = 4GB
));
$node->start;

my $n = 100_000;
my $bound = 8 * 1024 * 1024;
my $growth = 1024 * 1024;

$node->safe_psql(
	'postgres', q{
CREATE VIEW exec_bytes AS
  SELECT sum(c.total_bytes)::bigint AS bytes
  FROM pg_backend_memory_contexts c,
       (SELECT path, level FROM pg_backend_memory_contexts
        WHERE name = 'ExecutorState' ORDER BY level LIMIT 1) t
  WHERE t.path[t.level] = ANY (c.path);
});

# Is all of the permanent undo discarded?
my $discarded = q{
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT coalesce(bool_and(discard = insert), true)
        FROM pg_stat_get_undo_logs() WHERE persistence = 'permanent')};

# Every row gets a newest version with an undo record.
foreach my $am ('flux', 'heap')
{
	foreach my $p ('u', 'k')
	{
		my $t = "${p}_$am";
		$node->safe_psql(
			'postgres', qq{
CREATE TABLE $t (k int PRIMARY KEY, v int, w int) USING $am;
CREATE INDEX ${t}_w ON $t (w);
INSERT INTO $t SELECT g, g, g FROM generate_series(1, 3 * $n) g;
UPDATE $t SET v = v + 1;
ANALYZE $t;
});
	}
}

# Update the rows in (lo, hi] of a table with a sequential scan, and read the
# executor's memory in the same statement once the UPDATE is done.  Returns
# the number of rows updated and the bytes.
sub measure
{
	my ($t, $set, $lo, $hi) = @_;
	my $res = $node->safe_psql(
		'postgres', qq{
SET max_parallel_workers_per_gather = 0;
SET work_mem = '64kB';
SET enable_indexscan = off;
SET enable_bitmapscan = off;
WITH u AS (UPDATE $t SET $set WHERE k > $lo AND k <= $hi RETURNING 1),
     r AS MATERIALIZED (SELECT count(*) AS n FROM u)
SELECT n || ',' || (SELECT bytes FROM exec_bytes WHERE n IS NOT NULL) FROM r;
});
	return split /,/, $res;
}

foreach my $c ([ 'update', 'u', 'v = v + 1' ],
	[ 'key update', 'k', 'w = w + 10000000' ])
{
	my ($name, $p, $set) = @$c;
	foreach my $am ('flux', 'heap')
	{
		my $t = "${p}_$am";
		my %b;
		foreach my $round ([ 1, 0, $n ], [ 2, $n, 3 * $n ])
		{
			my ($r, $lo, $hi) = @$round;
			my $rows = $hi - $lo;
			ok( $node->poll_query_until('postgres', $discarded),
				"$name, $t, $rows rows: the undo is discarded");
			my ($count, $bytes) = measure($t, $set, $lo, $hi);
			is($count, $rows, "$name, $t, $rows rows: count");
			cmp_ok($bytes, '<', $bound,
				"$name, $t, $rows rows: executor memory bounded");
			note "$name, $t, $rows rows: $bytes bytes";
			$b{$r} = $bytes;
		}
		cmp_ok($b{2} - $b{1},
			'<', $growth,
			"$name, $t: executor memory does not grow with the rows");
	}
}

# The FLUX tables hold what their heap twins hold, through a sequential scan
# and through every index, and every FLUX btree index is intact.
my $have_amcheck = $node->check_extension('amcheck');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck') if $have_amcheck;
foreach my $p ('u', 'k')
{
	my @res;
	foreach my $am ('flux', 'heap')
	{
		my $t = "${p}_$am";
		my $q =
		  "SELECT count(*) || ':' || sum(k::bigint) || ':' || sum(v::bigint) || ':' || sum(w::bigint) FROM $t";
		push @res,
		  $node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; $q;\n"
			  . "SET enable_seqscan = off; SET enable_indexscan = on; $q WHERE k > 0;\n"
			  . "$q WHERE w > 0;");
	}
	is($res[0], $res[1], "${p}_flux: rows as heap");
	next unless $have_amcheck;
	foreach my $i ("${p}_flux_pkey", "${p}_flux_w")
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

unlike(slurp_file($node->logfile), qr/TRAP|PANIC/, 'no failed assertion');
$node->stop;
done_testing();
