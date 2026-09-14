# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A FLUX UPDATE that changes an indexed column in place inserts (k_new, TID)
# and delete-marks (k_old, TID).  That only describes the change when the two
# entries are different keys.  Two changes it cannot describe:
#
# - a change of an INCLUDE column only: the key, and so the entry, stays the
#   same, and the entry kept the old INCLUDE value;
# - a key change in an index whose opclass can call two different images
#   equal (numeric 3 and 3.00, float8 0 and -0): the new entry duplicated
#   the old one's (key, TID), or a tombstone revived under the other image
#   hid the row from a snapshot that saw the old one.
#
# Such an UPDATE must move the row to a new TID, as heap's non-HOT UPDATE
# does, so that every entry describes exactly one version.  The same holds
# when a row re-enters a partial index: the entry it gets back may be its own
# tombstone from before it left, re-formed from the new version.  amcheck
# (heapallindexed, and rootdescend + checkunique) runs after every step; heap
# is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_index_exact_keys');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
-- bt_index_check's error as text, '' if none: a background session can call
-- it in the snapshot it holds.
CREATE FUNCTION amcheck_err(idx regclass) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM bt_index_check(idx, true, false);
	RETURN '';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLERRM;
END $$;
});

# All checks of one index; '' when amcheck finds nothing.  With a writer in
# progress only bt_index_check, as bt_index_parent_check waits for its lock.
sub check_index
{
	my ($idx, $unique, $busy) = @_;
	my $sql = "SELECT bt_index_check('$idx', true, $unique)";
	$sql .= "; SELECT bt_index_parent_check('$idx', true, true, $unique)"
	  unless $busy;
	my ($rc, $out, $err) = $node->psql('postgres', $sql);
	return $err;
}

# Rows the index finds for a qual, by an index scan.
sub idx_rows
{
	my ($sql) = @_;
	return $node->safe_psql(
		'postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
$sql});
}

foreach my $am ('heap', 'flux')
{
	# INCLUDE column changed alone.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE ti_$am (k int, inc int) USING $am;
CREATE INDEX ti_${am}_k ON ti_$am (k) INCLUDE (inc);
INSERT INTO ti_$am SELECT g, g FROM generate_series(1, 5) g;
UPDATE ti_$am SET inc = 30 WHERE k = 3;
});
	is(check_index("ti_${am}_k", 'false'), '', "$am: INCLUDE-only UPDATE");
	$node->safe_psql('postgres', "UPDATE ti_$am SET inc = 3 WHERE k = 3");
	is(check_index("ti_${am}_k", 'false'),
		'', "$am: INCLUDE value changed back (A->B->A)");

	# The same on a unique index, with a rollback and a real duplicate.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE tu_$am (k int, inc int) USING $am;
CREATE UNIQUE INDEX tu_${am}_k ON tu_$am (k) INCLUDE (inc);
INSERT INTO tu_$am SELECT g, g FROM generate_series(1, 5) g;
});
	my ($rc, $out, $err) =
	  $node->psql('postgres', "UPDATE tu_$am SET inc = 30 WHERE k = 3");
	is($rc . $err, '0', "$am: unique INCLUDE-only UPDATE raises no conflict");
	is(check_index("tu_${am}_k", 'true'),
		'', "$am: unique INCLUDE-only UPDATE");

	my $bg = $node->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe("UPDATE tu_$am SET inc = 300 WHERE k = 3");
	is(check_index("tu_${am}_k", 'true', 1),
		'', "$am: unique INCLUDE-only UPDATE in progress");
	$bg->query_safe('ROLLBACK');
	$bg->quit;
	is(check_index("tu_${am}_k", 'true'),
		'', "$am: unique INCLUDE-only UPDATE rolled back");
	is($node->safe_psql('postgres', "SELECT inc FROM tu_$am WHERE k = 3"),
		'30', "$am: rollback restored the INCLUDE value");

	$node->safe_psql('postgres', "UPDATE tu_$am SET inc = 3 WHERE k = 3");
	is(check_index("tu_${am}_k", 'true'), '', "$am: unique INCLUDE A->B->A");
	($rc, $out, $err) =
	  $node->psql('postgres', "INSERT INTO tu_$am VALUES (3, 99)");
	like(
		$err,
		qr/duplicate key value/,
		"$am: a real duplicate is still refused");

	# A key change keeps an int-keyed row at its TID; a change of an INCLUDE
	# column moves it, with or without a key change (a later change back to
	# the old key would revive that key's entry with the new INCLUDE value).
	my $ctid =
	  $node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 3");
	$node->safe_psql('postgres', "UPDATE tu_$am SET k = 13 WHERE k = 3");
	is( $node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 13")
		  eq $ctid ? 'same' : 'moved',
		$am eq 'flux' ? 'same' : 'moved',
		"$am: key change in an int index with INCLUDE columns");
	$ctid =
	  $node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 13");
	$node->safe_psql('postgres',
		"UPDATE tu_$am SET k = 103, inc = 103 WHERE k = 13");
	isnt(
		$node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 103"),
		$ctid,
		"$am: a key and INCLUDE change moves the row");
	$ctid =
	  $node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 4");
	$node->safe_psql('postgres', "UPDATE tu_$am SET inc = 40 WHERE k = 4");
	isnt($node->safe_psql('postgres', "SELECT ctid FROM tu_$am WHERE k = 4"),
		$ctid, "$am: an INCLUDE-only change moves the row");
	is(check_index("tu_${am}_k", 'true'), '', "$am: key and INCLUDE change");
	is(idx_rows("SELECT k, inc FROM tu_$am WHERE k IN (3, 103)"),
		'103|103', "$am: index finds the row under its new key only");

	# numeric: 3 and 3.00 are equal keys with different images.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE tn_$am (id int, n numeric) USING $am;
CREATE INDEX tn_${am}_n ON tn_$am (n);
INSERT INTO tn_$am SELECT g, g FROM generate_series(1, 5) g;
UPDATE tn_$am SET n = 3.00 WHERE id = 3;
});
	is(check_index("tn_${am}_n", 'false'), '', "$am: numeric 3 -> 3.00");
	is(idx_rows("SELECT id, n FROM tn_$am WHERE n = 3"),
		'3|3.00', "$am: index scan sees 3.00");

	# 3 -> 4 -> 3.0 while a snapshot from before both UPDATEs is open.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE tr_$am (id int, n numeric) USING $am;
CREATE INDEX tr_${am}_n ON tr_$am (n);
INSERT INTO tr_$am SELECT g, g FROM generate_series(1, 5) g;
});
	my $rr = $node->background_psql('postgres');
	$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr->query_safe("SELECT count(*) FROM tr_$am");
	$node->safe_psql(
		'postgres', qq{
UPDATE tr_$am SET n = 4 WHERE id = 3;
UPDATE tr_$am SET n = 3.0 WHERE id = 3;
});
	is( $rr->query_safe(
			qq{SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SELECT id, n FROM tr_$am WHERE n = 3}),
		'3|3',
		"$am: old snapshot finds the old version through the index");
	$rr->query_safe('COMMIT');
	$rr->quit;
	is(idx_rows("SELECT id, n FROM tr_$am WHERE n = 3"),
		'3|3.0', "$am: new snapshot finds the new version");
	is(check_index("tr_${am}_n", 'false'), '', "$am: numeric 3 -> 4 -> 3.0");

	# float8: 0 and -0 are equal keys with different images.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE tf_$am (id int, f float8) USING $am;
CREATE UNIQUE INDEX tf_${am}_f ON tf_$am (f);
INSERT INTO tf_$am SELECT g, g FROM generate_series(0, 5) g;
UPDATE tf_$am SET f = '-0' WHERE id = 0;
});
	is(check_index("tf_${am}_f", 'true'), '', "$am: float8 0 -> -0");
	is(idx_rows("SELECT id, f FROM tf_$am WHERE f = 0"),
		'0|-0', "$am: index scan sees -0");

	# A row leaves a partial index and comes back with an equal key of
	# another image, or another INCLUDE value, while a snapshot from before
	# it left is open.  Leaving delete-marks its entry; coming back finds
	# that tombstone for the same (key, TID) and revives it re-formed from
	# the new version, so the old snapshot loses the image it sees.  Each
	# index is checked through the old snapshot (index scan against seqscan,
	# and amcheck's heapallindexed from that snapshot) and afterwards.
	# A key that changes and comes back with another INCLUDE value revives
	# its tombstone the same way, and so does one in an index with no
	# predicate.
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE kc_$am (id int, n int, inc int) USING $am;
CREATE INDEX kc_${am}_i ON kc_$am (n) INCLUDE (inc);
INSERT INTO kc_$am SELECT g, g, g FROM generate_series(1, 5) g;
});
	$rr = $node->background_psql('postgres');
	$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr->query_safe("SELECT count(*) FROM kc_$am");
	$node->safe_psql(
		'postgres', qq{
UPDATE kc_$am SET n = 4 WHERE id = 3;
UPDATE kc_$am SET n = 3, inc = 30 WHERE id = 3;
});
	is($rr->query_safe("SELECT amcheck_err('kc_${am}_i')"),
		'', "$am: key cycle with an INCLUDE change, from the old snapshot");
	$rr->query_safe('COMMIT');
	$rr->quit;
	is(check_index("kc_${am}_i", 'false'),
		'', "$am: key cycle with an INCLUDE change");

	foreach my $case (
		[ 'pn', 'n numeric', '(n)', 'n = 3', 'n = 3.0', '3' ],
		[ 'pe', 'n numeric', '((n + 0))', 'n + 0 = 3', 'n = 3.0', '3' ],
		[
			'pi',
			'n int, inc int',
			'(n) INCLUDE (inc)',
			'n = 3', 'inc = 30', '3|3'
		])
	{
		my ($t, $cols, $def, $qual, $set, $old) = @$case;
		my $tab = "${t}_$am";
		my $out = $cols =~ /inc/ ? 'n, inc' : 'n';
		$node->safe_psql(
			'postgres', qq{
CREATE TABLE $tab (id int, $cols, active bool) USING $am;
CREATE INDEX ${tab}_i ON $tab $def WHERE active;
INSERT INTO $tab (id, n, active) SELECT g, g, true FROM generate_series(1, 5) g;
});
		$node->safe_psql('postgres', "UPDATE $tab SET inc = n")
		  if $cols =~ /inc/;
		my $rr = $node->background_psql('postgres');
		$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
		$rr->query_safe("SELECT count(*) FROM $tab");
		$node->safe_psql(
			'postgres', qq{
UPDATE $tab SET active = false WHERE id = 3;
UPDATE $tab SET active = true, $set WHERE id = 3;
});
		my $q = "SELECT $out FROM $tab WHERE $qual AND active";
		is( $rr->query_safe(
				qq{SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = off; $q}),
			$old,
			"$am $t: old snapshot finds the row it sees through the index");
		is( $rr->query_safe(
				qq{SET enable_seqscan = on; SET enable_indexscan = off;
SET enable_bitmapscan = off; $q}),
			$old,
			"$am $t: old snapshot seqscan");
		is($rr->query_safe("SELECT amcheck_err('${tab}_i')"),
			'', "$am $t: heapallindexed from the old snapshot");
		$rr->query_safe('COMMIT');
		$rr->quit;
		is(check_index("${tab}_i", 'false'), '', "$am $t: amcheck after");
		is(idx_rows("SELECT count(*) FROM $tab WHERE $qual AND active"),
			'1', "$am $t: new snapshot finds the row once");
	}
}

$node->stop;
done_testing();
