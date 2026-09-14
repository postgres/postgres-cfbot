
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX_TUPLE_UNCOMMITTED is a hint, as heap's hint bits are: nothing may
# depend on its having been cleared, and a set flag does not prove that the
# writer is still running.  The writer xids in the header and CLOG decide.
# Heap is the control for every case.
#
# (a) A transaction's memory does not grow with the rows it inserts.  FLUX
#     used to remember every inserted TID until commit, so that commit could
#     clear the hint on it: about 50 bytes a row (80 inside a savepoint), and
#     a scale-17000 pgbench load ran the backend out of memory.  Nor with
#     the rows it updates or deletes: the command ids of those, which heap
#     keeps in the tuple, went to a map with an entry per row, 42 bytes.
# (b, c) VACUUM reclaims a DELETE that a released savepoint did, or that
#     COMMIT PREPARED committed.  Commit cleared the hint only for its own
#     top-level xid, in its own backend, and VACUUM took a set hint for a
#     delete in progress, so neither was ever reclaimed.
# (d) A transaction that changes more pages than commit revisits: the rows it
#     leaves hinted behave as committed ones, and those of a rolled-back
#     savepoint are invisible and reclaimed.  (A guard on the old code, which
#     revisited every page.)
# (e) An old snapshot reads the old versions through index and bitmap scans
#     after a released-savepoint or a prepared UPDATE.  (A guard.)
# Our own writes keep the hint until commit, which the command id tests use:
#     a cursor and an UPDATE do not see the rows a later command wrote.  (A
#     guard.)
# (f) After crash recovery a prepared DELETE, which redo replays without the
#     hint, still makes a concurrent DELETE or UPDATE wait for it.  FLUX took
#     the delete mark without the hint for a committed delete and skipped the
#     row, which the prepared transaction then rolled back.
# (g) VACUUM reclaims a committed DELETE whose page image redo restored with
#     the hint set: commit cleared it without WAL, and the crash lost that.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_prepared_transactions = 5
));
$node->start;

# The memory of the transaction: every context under TopTransactionContext.
my $memq = q{SELECT sum(c.total_bytes)
  FROM pg_backend_memory_contexts c, pg_backend_memory_contexts t
 WHERE t.name = 'TopTransactionContext' AND t.path[t.level] = ANY (c.path)};

# (a)
my $n = 1000000;
my $file = $node->basedir . '/rows.txt';
$file =~ s{\\}{/}g if $PostgreSQL::Test::Utils::windows_os;
$node->safe_psql('postgres',
	"COPY (SELECT generate_series(1, $n)) TO '$file'");
foreach my $am ('heap', 'flux')
{
	foreach my $mode ('insert', 'copy', 'savepoint')
	{
		my $load =
		  $mode eq 'copy'
		  ? "COPY m FROM '$file'"
		  : "INSERT INTO m SELECT generate_series(1, $n)";
		my ($sp, $release) =
		  $mode eq 'savepoint' ? ('SAVEPOINT s;', 'RELEASE s;') : ('', '');
		$node->safe_psql('postgres',
			"DROP TABLE IF EXISTS m; CREATE TABLE m (id int) USING $am");
		my ($m1, $m2) = split /\n/,
		  $node->safe_psql('postgres',
			"BEGIN; $sp $load; $memq; $load; $memq; $release COMMIT;");
		note "$am $mode: $m1 bytes after $n rows, $m2 after " . 2 * $n;
		cmp_ok(
			$m2, '<',
			8 * 1024 * 1024,
			"$am $mode: transaction memory stays small");
		cmp_ok(
			$m2 - $m1, '<',
			1024 * 1024,
			"$am $mode: transaction memory does not grow with the rows");
		is($node->safe_psql('postgres', 'SELECT count(*) FROM m'),
			2 * $n, "$am $mode: every row committed");
	}

	# UPDATE and DELETE of the rows, half and half.
	foreach my $op ('UPDATE m SET id = -id', 'DELETE FROM m')
	{
		$node->safe_psql('postgres',
				"DROP TABLE m; CREATE TABLE m (id int) USING $am; "
			  . "INSERT INTO m SELECT generate_series(1, 2 * $n)");
		my ($m1, $m2) = split /\n/,
		  $node->safe_psql('postgres',
				"BEGIN; $op WHERE id BETWEEN 1 AND $n; $memq; "
			  . "$op WHERE id > $n; $memq; COMMIT;");
		note "$am $op: $m1 bytes after $n rows, $m2 after " . 2 * $n;
		cmp_ok(
			$m2, '<',
			8 * 1024 * 1024,
			"$am $op: transaction memory stays small");
		cmp_ok(
			$m2 - $m1, '<',
			1024 * 1024,
			"$am $op: transaction memory does not grow with the rows");
	}
}

# (b), (c), and a plain DELETE as the control
foreach my $am ('heap', 'flux')
{
	foreach my $how ('plain', 'savepoint', 'prepared')
	{
		my $t = "r_${am}_$how";
		my $fill =
		  "INSERT INTO $t SELECT g, repeat('x', 200) FROM generate_series(1, 20000) g";
		$node->safe_psql('postgres',
			"CREATE TABLE $t (id int, pad text) USING $am; $fill");
		my $size =
		  $node->safe_psql('postgres', "SELECT pg_relation_size('$t')");
		my $del =
			$how eq 'plain' ? "DELETE FROM $t;"
		  : $how eq 'savepoint'
		  ? "BEGIN; SAVEPOINT s; DELETE FROM $t; RELEASE s; COMMIT;"
		  : "BEGIN; DELETE FROM $t; PREPARE TRANSACTION '$t'; COMMIT PREPARED '$t';";
		$node->safe_psql('postgres', "$del VACUUM $t;");
		is($node->safe_psql('postgres', "SELECT pg_relation_size('$t')"),
			'0', "$am $how: VACUUM reclaims a committed DELETE");
		$node->safe_psql('postgres', $fill);
		cmp_ok($node->safe_psql('postgres', "SELECT pg_relation_size('$t')"),
			'<=', $size, "$am $how: the rows inserted again reuse the space");
	}
}

# (d) and (e): with 1MB of shared buffers commit revisits a few pages only.
my $small = PostgreSQL::Test::Cluster->new('small');
$small->init;
$small->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_buffers = 1MB
max_connections = 10
max_prepared_transactions = 5
));
$small->start;
my $rows = 20000;
my $sum = $rows * ($rows + 1) / 2;
foreach my $am ('heap', 'flux')
{
	# (d) One transaction inserts and updates rows on some 600 pages.  The
	# first statements after it are writers, which meet the rows before any
	# reader could have cleared their hints.
	$small->safe_psql(
		'postgres', qq(
CREATE TABLE d_$am (id int PRIMARY KEY, v int, pad text) USING $am;
BEGIN;
INSERT INTO d_$am SELECT g, 0, repeat('x', 200) FROM generate_series(1, $rows) g;
UPDATE d_$am SET v = 1;
COMMIT;
));
	my ($ret, $out, $err) = $small->psql(
		'postgres', qq(
SET lock_timeout = '10s';
BEGIN ISOLATION LEVEL REPEATABLE READ;
DELETE FROM d_$am WHERE id % 2 = 0;
UPDATE d_$am SET v = v + 1;
COMMIT;
VACUUM d_$am;
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM (SELECT 1 FROM d_$am FOR UPDATE NOWAIT) s;
SELECT sum(v) FROM d_$am;
DELETE FROM d_$am;
COMMIT;
VACUUM d_$am;
SELECT pg_relation_size('d_$am');
));
	is($ret, 0, "$am: later writers neither wait nor fail on a big commit")
	  or diag $err;
	is( $out,
		$rows / 2 . "\n" . $rows . "\n0",
		"$am: they see, change and remove every row, and VACUUM reclaims");

	# Inserts of a rolled-back savepoint, on many pages; the parent commits.
	$small->safe_psql(
		'postgres', qq(
CREATE TABLE s_$am (id int, pad text) USING $am;
CREATE TABLE one_$am (id int) USING $am;
BEGIN;
SAVEPOINT s;
INSERT INTO s_$am SELECT g, repeat('x', 200) FROM generate_series(1, $rows) g;
ROLLBACK TO s;
INSERT INTO one_$am VALUES (1);
COMMIT;
));
	is($small->safe_psql('postgres', "SELECT count(*) FROM s_$am"),
		'0', "$am: a rolled-back savepoint's rows are invisible");
	$small->safe_psql('postgres', "VACUUM s_$am");
	is($small->safe_psql('postgres', "SELECT pg_relation_size('s_$am')"),
		'0', "$am: VACUUM reclaims them");

	# (e) An old snapshot, then UPDATEs of indexed rows that a released
	# savepoint and a prepared transaction commit.
	$small->safe_psql(
		'postgres', qq(
CREATE TABLE e_$am (id int PRIMARY KEY, v int, w int) USING $am;
CREATE INDEX ON e_$am (v);
INSERT INTO e_$am SELECT g, g, g FROM generate_series(1, $rows) g;
VACUUM ANALYZE e_$am;
));
	my $old = $small->background_psql('postgres');
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe("SELECT count(*) FROM e_$am");
	$small->safe_psql(
		'postgres', qq(
BEGIN;
SAVEPOINT s;
UPDATE e_$am SET v = v + $rows, w = w + 1;
RELEASE s;
COMMIT;
BEGIN;
UPDATE e_$am SET v = v + $rows, w = w + 1;
PREPARE TRANSACTION 'e_$am';
COMMIT PREPARED 'e_$am';
));
	foreach my $scan ('Index Scan', 'Bitmap Heap Scan', 'Seq Scan')
	{
		my $set =
		  $scan eq 'Index Scan'
		  ? 'SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = on;'
		  : $scan eq 'Bitmap Heap Scan'
		  ? 'SET enable_seqscan = off; SET enable_bitmapscan = on; SET enable_indexscan = off;'
		  : 'SET enable_seqscan = on; SET enable_bitmapscan = off; SET enable_indexscan = off;';
		$old->query_safe($set);
		foreach my $col ('id', 'v')
		{
			my $q =
			  "SELECT sum(v), sum(w) FROM e_$am WHERE $col BETWEEN 1 AND $rows";
			like($old->query_safe("EXPLAIN (COSTS OFF) $q"),
				qr/\Q$scan\E/, "$am: the plan for $col is a $scan");
			is($old->query_safe($q),
				"$sum|$sum",
				"$am: an old snapshot reads the old rows by $col, $scan");
		}
	}
	$old->query_safe('COMMIT');
	$old->quit;
	is( $small->safe_psql(
			'postgres',
			"SELECT sum(v) - 2 * $rows * $rows, sum(w) FROM e_$am"),
		"$sum|" . ($sum + 2 * $rows),
		"$am: a new snapshot reads the new rows");
}

# Our own writes keep the hint until we commit, whoever reads them: the
# command id tests depend on it.  A cursor opened before our UPDATE still
# reads the old rows, and an UPDATE does not update its own output again.
foreach my $am ('heap', 'flux')
{
	$node->safe_psql('postgres',
		"CREATE TABLE o_$am (id int PRIMARY KEY, v int, pad text) USING $am; "
		  . "INSERT INTO o_$am SELECT g, 0, 'x' FROM generate_series(1, 2000) g"
	);
	is( $node->safe_psql(
			'postgres', qq(
BEGIN;
DECLARE c CURSOR FOR SELECT sum(v) FROM o_$am;
UPDATE o_$am SET v = v + 1;
SELECT count(*) FROM o_$am WHERE v = 1;
UPDATE o_$am SET v = v + 1, pad = repeat('y', 500);
FETCH c;
SELECT sum(v) FROM o_$am;
COMMIT;
)),
		"2000\n0\n4000",
		"$am: own writes keep their command ids");

	# The same for our own DELETE: a cursor opened before it still reads the
	# rows, and a later command does not.
	is( $node->safe_psql(
			'postgres', qq(
BEGIN;
DECLARE c CURSOR FOR SELECT count(*), sum(v) FROM o_$am;
DELETE FROM o_$am WHERE id % 2 = 0;
FETCH c;
SELECT count(*) FROM o_$am;
ROLLBACK;
)),
		"2000|4000\n1000",
		"$am: own DELETE keeps its command id");
}

# (f) and (g): crash recovery.
foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE f_$am (id int, v int) USING $am;
INSERT INTO f_$am SELECT g, 0 FROM generate_series(1, 10) g;
CREATE TABLE g_$am (id int, pad text) USING $am;
INSERT INTO g_$am SELECT g, repeat('x', 200) FROM generate_series(1, 20000) g;
-- set the hint bits now, so that no hint takes the page image below
SELECT count(*) FROM g_$am;
CHECKPOINT;
-- the page image goes with this change, so the DELETE below is replayed
UPDATE f_$am SET v = 1 WHERE id = 2;
BEGIN;
DELETE FROM f_$am WHERE id = 1;
PREPARE TRANSACTION 'f_$am';
-- each page's image goes with its first deleted row, hint and all
DELETE FROM g_$am;
));
}
$node->stop('immediate');
$node->start;
foreach my $am ('heap', 'flux')
{
	foreach my $op (
		"DELETE FROM f_$am WHERE id = 1",
		"UPDATE f_$am SET v = 2 WHERE id = 1",
		"SELECT * FROM f_$am WHERE id = 1 FOR UPDATE")
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "SET lock_timeout = '1s'; $op");
		like(
			$err,
			qr/lock timeout/,
			"$am: after recovery, \"$op\" waits for a prepared DELETE");
	}
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'f_$am'");
	is( $node->safe_psql(
			'postgres', "DELETE FROM f_$am WHERE id = 1 RETURNING id"),
		'1',
		"$am: the row comes back when the prepared DELETE rolls back");

	$node->safe_psql('postgres', "VACUUM g_$am");
	is($node->safe_psql('postgres', "SELECT pg_relation_size('g_$am')"),
		'0', "$am: VACUUM reclaims a DELETE replayed from page images");
}

done_testing();
