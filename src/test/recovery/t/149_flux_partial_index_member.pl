# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A partial index answers a query only for rows that satisfy its predicate:
# the planner drops a qual the predicate implies, and a unique or exclusion
# check counts only member rows.  A FLUX in-place UPDATE keeps the row at
# its TID, so a row whose membership changes keeps the entry its earlier
# version had, delete-marked, and the reader's recheck compares only the
# entry's key with the visible version's.  Every query here has a heap
# control; amcheck runs on every index afterwards.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_partial_index_member');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');

# Run a query with index scans forced (no seqscan, no bitmap scan).
sub idx
{
	my ($sql) = @_;
	return $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = off; SET enable_sort = off;
$sql});
}

# The same through a bitmap heap scan.
sub bitmap
{
	my ($sql, $work_mem) = @_;
	return $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off; SET enable_indexscan = off;
SET enable_indexonlyscan = off; SET work_mem = '$work_mem';
$sql});
}

sub check_indexes
{
	my ($tab) = @_;
	my $res = $node->safe_psql(
		'postgres', qq{
SELECT string_agg(c.relname, ',') FROM pg_index i
  JOIN pg_class c ON c.oid = i.indexrelid
  JOIN pg_am a ON a.oid = c.relam
 WHERE i.indrelid = '$tab'::regclass AND a.amname = 'btree'
   AND NOT i.indisexclusion});
	my $err = '';
	foreach my $idx (split /,/, $res)
	{
		my ($rc, $out, $e) = $node->psql('postgres',
			"SELECT bt_index_parent_check('$idx', true, true, true)");
		$err .= $e;
	}
	return $err;
}

foreach my $am ('heap', 'flux')
{
	# A row leaves the index's predicate, its key unchanged.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE l_$am (id int, n int, active bool) USING $am;
CREATE INDEX l_${am}_i ON l_$am (n) WHERE active;
INSERT INTO l_$am SELECT g, g, true FROM generate_series(1, 5) g;
UPDATE l_$am SET active = false WHERE id IN (1, 3);
});
	is(idx("SELECT id, active FROM l_$am WHERE n = 3 AND active"),
		'', "$am: a row that left the predicate is not found by its key");
	is( idx("SELECT count(*) FROM l_$am WHERE n BETWEEN 1 AND 5 AND active"),
		'3', "$am: nor by a range scan");
	is(idx("SELECT min(n) FROM l_$am WHERE active"),
		'2', "$am: min() through the partial index");
	is(idx("SELECT id FROM l_$am WHERE active ORDER BY n LIMIT 1"),
		'2', "$am: ORDER BY through the partial index");
	is( idx(qq{SET enable_hashjoin = off; SET enable_nestloop = off;
SELECT count(*) FROM l_$am a JOIN generate_series(1, 5) g ON a.n = g
 WHERE a.active}),
		'3',
		"$am: merge join through the partial index");
	is(bitmap("SELECT count(*) FROM l_$am WHERE n <= 5 AND active", '4MB'),
		'3', "$am: exact bitmap scan");
	is(check_indexes("l_$am"), '', "$am: amcheck after leaving");

	# Lossy bitmap pages: enough rows that 64kB cannot keep every TID.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE lb_$am (id int, n int, active bool) USING $am;
CREATE INDEX lb_${am}_i ON lb_$am (n) WHERE active;
INSERT INTO lb_$am SELECT g, g % 10, true FROM generate_series(1, 100000) g;
UPDATE lb_$am SET active = false WHERE id % 10 = 3;
});
	is( bitmap(
			"SELECT count(*), count(*) FILTER (WHERE NOT active) FROM lb_$am WHERE n = 3 AND active",
			'64kB'),
		'0|0',
		"$am: lossy bitmap scan");

	# A row leaves the predicate and changes its key.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE lk_$am (id int, n int, active bool) USING $am;
CREATE INDEX lk_${am}_i ON lk_$am (n) WHERE active;
INSERT INTO lk_$am SELECT g, g, true FROM generate_series(1, 5) g;
UPDATE lk_$am SET active = false, n = 30 WHERE id = 3;
});
	is(idx("SELECT count(*) FROM lk_$am WHERE n IN (3, 30) AND active"),
		'0', "$am: left the predicate with another key");
	is(check_indexes("lk_$am"), '', "$am: amcheck after leaving with a key");

	# A predicate on another column, through an expression.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE le_$am (id int, n int) USING $am;
CREATE INDEX le_${am}_i ON le_$am (id) WHERE n % 2 = 1;
INSERT INTO le_$am SELECT g, g FROM generate_series(1, 5) g;
UPDATE le_$am SET n = 4 WHERE id = 3;
});
	is(idx("SELECT id, n FROM le_$am WHERE id = 3 AND n % 2 = 1"),
		'', "$am: left an expression predicate");
	is(check_indexes("le_$am"), '', "$am: amcheck after an expression left");

	# A snapshot that sees the row outside the predicate, after it came
	# back with the same key.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE r_$am (id int, n int, active bool) USING $am;
CREATE INDEX r_${am}_i ON r_$am (n) WHERE active;
INSERT INTO r_$am SELECT g, g, true FROM generate_series(1, 5) g;
UPDATE r_$am SET active = false WHERE id = 3;
});
	my $rr = $node->background_psql('postgres');
	$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr->query_safe("SELECT count(*) FROM r_$am");
	$node->safe_psql('postgres',
		"UPDATE r_$am SET active = true WHERE id = 3");
	is( $rr->query_safe(
			qq{SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT id, active FROM r_$am WHERE n = 3 AND active}),
		'',
		"$am: snapshot from outside the predicate after re-entry");
	$rr->query_safe('COMMIT');
	$rr->quit;
	is(idx("SELECT id, active FROM r_$am WHERE n = 3 AND active"),
		'3|t', "$am: re-entered row is found");
	is(check_indexes("r_$am"), '', "$am: amcheck after re-entry");

	# A partial unique index counts member rows only.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE u_$am (id int, n int, active bool) USING $am;
CREATE UNIQUE INDEX u_${am}_i ON u_$am (n) WHERE active;
INSERT INTO u_$am SELECT g, g, true FROM generate_series(1, 5) g;
UPDATE u_$am SET active = false WHERE id IN (2, 3);
});
	my ($rc, $out, $err) =
	  $node->psql('postgres', "INSERT INTO u_$am VALUES (13, 3, true)");
	is($rc . $err, '0', "$am: unique key free after its row left");
	is( $node->safe_psql(
			'postgres', qq{
INSERT INTO u_$am VALUES (12, 2, true) ON CONFLICT (n) WHERE active DO NOTHING
RETURNING id}),
		'12',
		"$am: ON CONFLICT finds no conflict for a row that left");
	($rc, $out, $err) =
	  $node->psql('postgres', "UPDATE u_$am SET active = true WHERE id = 3");
	like(
		$err,
		qr/duplicate key value/,
		"$am: a row coming back to a taken key conflicts");
	is(check_indexes("u_$am"), '', "$am: amcheck after unique membership");

	# An exclusion constraint with a predicate, and a hash index.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE x_$am (id int, n int, active bool,
  EXCLUDE USING btree (n WITH =) WHERE (active)) USING $am;
CREATE INDEX x_${am}_h ON x_$am USING hash (n) WHERE active;
INSERT INTO x_$am SELECT g, g, true FROM generate_series(1, 5) g;
UPDATE x_$am SET active = false WHERE id = 3;
});
	($rc, $out, $err) =
	  $node->psql('postgres', "INSERT INTO x_$am VALUES (13, 3, true)");
	is($rc . $err, '0', "$am: exclusion constraint free after its row left");
	is(idx("SELECT id FROM x_$am WHERE n = 3 AND active"),
		'13', "$am: hash partial index finds the member only");
}

$node->stop;
done_testing();
