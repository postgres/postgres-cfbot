
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX deletes a row version's external TOAST values when the version is
# deleted, as heap_delete does: an MVCC delete in the TOAST table by the
# deleting transaction.  Every path that retires a version must do so once,
# the values must come back with a rolled-back DELETE, a snapshot that still
# reads the deleted version must still detoast it across VACUUM of the table
# and of its TOAST table, and logical decoding must see what it sees for heap.
# Every case runs on a heap twin with the same expectations.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_toast_delete');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf',
	"autovacuum = off\nmax_prepared_transactions = 5\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

# Row id's value: about 10 kB, stored uncompressed out of line.
sub val { my ($id) = @_; return "($id || ':' || repeat('x', 10000))" }

sub make
{
	my ($t, $am, $extra) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, k int, big text) USING $am
	WITH (autovacuum_enabled = off);
ALTER TABLE $t ALTER big SET STORAGE EXTERNAL;
CREATE UNIQUE INDEX ${t}_id ON $t (id);
@{[$extra // '']}
INSERT INTO $t SELECT g, g, @{[val('g')]} FROM generate_series(1, 4) g;
});
}

sub toast_of
{
	my ($t) = @_;
	return $node->safe_psql('postgres',
		"SELECT reltoastrelid::regclass FROM pg_class WHERE oid = '$t'::regclass"
	);
}

# VACUUM the table and its TOAST table.
sub vacuum_both
{
	my ($t) = @_;
	$node->safe_psql('postgres', "VACUUM $t");
	$node->safe_psql('postgres', 'VACUUM ' . toast_of($t));
}

# Rows with intact values; TOAST values an MVCC snapshot sees; and whether
# the TOAST pages hold nothing else (every deleted chunk was removed).
sub state_of
{
	my ($t) = @_;
	my $toast = toast_of($t);
	return $node->safe_psql(
		'postgres', qq{
SELECT string_agg(id || '/' || k || '/' || (big = @{[val('id')]})::text, ' '
				  ORDER BY id) FROM $t;
SELECT count(DISTINCT chunk_id) FROM $toast;
SELECT (SELECT count(*)
		FROM generate_series(0, pg_relation_size('$toast') /
			 current_setting('block_size')::int - 1) b,
			 heap_page_items(get_raw_page('$toast', b::int))
		WHERE lp_flags <> 0) = (SELECT count(*) FROM $toast)});
}

my $all4 = '1/1/true 2/2/true 3/3/true 4/4/true';

foreach my $am ('heap', 'flux')
{
	# --- A snapshot that reads deleted and superseded versions. ---
	# Committed DELETE, in-place UPDATE of the TOASTed column, and a
	# key-changing UPDATE that moves the row (a hash index cannot be
	# delete-marked, so FLUX moves it as heap does), each with a snapshot
	# taken before it that reads the old version across VACUUM of the table
	# and of the TOAST table.
	my $t = "old_$am";
	make($t, $am, "CREATE INDEX ${t}_k ON $t USING hash (k);");
	my $rr = $node->background_psql('postgres');
	$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr->query_safe("SELECT count(*) FROM $t");
	$node->safe_psql(
		'postgres', qq{
DELETE FROM $t WHERE id = 1;
UPDATE $t SET big = 'new' || repeat('y', 10000) WHERE id = 2;
UPDATE $t SET k = 103 WHERE id = 3;
UPDATE $t SET k = 104, big = 'new' || repeat('z', 10000) WHERE id = 4;
});
	my $read_old = qq{SELECT string_agg(id || '/' || k || '/' ||
						(big = @{[val('id')]})::text, ' ' ORDER BY id) FROM $t};
	is($rr->query_safe($read_old),
		$all4, "$am: old snapshot reads the old versions before VACUUM");
	vacuum_both($t);
	is($rr->query_safe($read_old),
		$all4, "$am: and after VACUUM of the table and its TOAST table");
	$rr->query_safe('COMMIT');
	$rr->quit;
	vacuum_both($t);
	is( state_of($t),
		"2/2/false 3/103/true 4/104/false\n3\nt",
		"$am: once the snapshot is gone the old values are reclaimed");

	# --- Partition move and MERGE DELETE. ---
	my $p = "part_$am";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $p (id int, k int, big text) PARTITION BY RANGE (k);
CREATE TABLE ${p}_1 PARTITION OF $p FOR VALUES FROM (0) TO (100) USING $am;
CREATE TABLE ${p}_2 PARTITION OF $p FOR VALUES FROM (100) TO (1000) USING $am;
ALTER TABLE $p ALTER big SET STORAGE EXTERNAL;
INSERT INTO $p SELECT g, g, @{[val('g')]} FROM generate_series(1, 4) g;
UPDATE $p SET k = 500 WHERE id = 1;
MERGE INTO $p USING (VALUES (2)) s(id) ON $p.id = s.id
	WHEN MATCHED THEN DELETE;
});
	vacuum_both("${p}_1");
	vacuum_both("${p}_2");
	is( state_of("${p}_1"),
		"3/3/true 4/4/true\n2\nt",
		"$am: partition moved from and MERGE DELETE: two values left");
	is(state_of("${p}_2"), "1/500/true\n1\nt",
		"$am: partition moved to: one value");

	# --- DELETE that waits and retries. ---
	# The first deleter rolls back: the waiter deletes the row, once.  An
	# in-place UPDATE of another column commits: the waiter deletes the
	# updated version, once.
	$t = "wait_$am";
	make($t, $am);
	my $s1 = $node->background_psql('postgres');
	my $s2 = $node->background_psql('postgres');
	foreach my $c (
		[ 'BEGIN; DELETE FROM T WHERE id = 1', 'ROLLBACK', 1 ],
		[ 'BEGIN; UPDATE T SET k = 20 WHERE id = 2', 'COMMIT', 2 ])
	{
		my ($first, $end, $id) = @$c;
		(my $f = $first) =~ s/\bT\b/$t/g;
		$s1->query_safe($f);
		$s2->query_until(qr/start/,
			"\\echo start\nDELETE FROM $t WHERE id = $id;\n");
		$node->poll_query_until(
			'postgres', qq{
SELECT count(*) > 0 FROM pg_stat_activity
WHERE wait_event_type = 'Lock' AND query LIKE 'DELETE FROM $t%'}
		) or die 'DELETE did not wait';
		$s1->query_safe($end);
		is($s2->query_safe("SELECT count(*) FROM $t WHERE id = $id"),
			'0', "$am: waiting DELETE of row $id went ahead after $end");
	}
	$s1->quit;
	$s2->quit;
	vacuum_both($t);
	is( state_of($t),
		"3/3/true 4/4/true\n2\nt",
		"$am: each waited DELETE deleted its values once");

	# --- Rollbacks: the values must be live again. ---
	$t = "rb_$am";
	make($t, $am);
	$node->safe_psql(
		'postgres', qq{
BEGIN; DELETE FROM $t WHERE id = 1; ROLLBACK;
BEGIN; SAVEPOINT s; DELETE FROM $t WHERE id = 2; ROLLBACK TO s; COMMIT;
BEGIN; DELETE FROM $t WHERE id = 3; PREPARE TRANSACTION 'rb_$am';
ROLLBACK PREPARED 'rb_$am';
BEGIN; DELETE FROM $t WHERE id = 4; PREPARE TRANSACTION 'cp_$am';
COMMIT PREPARED 'cp_$am';
});
	vacuum_both($t);
	is( state_of($t),
		"1/1/true 2/2/true 3/3/true\n3\nt",
		"$am: aborted, savepoint, prepared DELETEs keep their values; a committed prepared one does not"
	);

	# --- CLUSTER copies a recently dead row. ---
	$t = "cl_$am";
	make($t, $am);
	$rr = $node->background_psql('postgres');
	$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr->query_safe('SELECT 1 FROM pg_class LIMIT 1');
	$node->safe_psql('postgres', "DELETE FROM $t WHERE id = 2");
	$node->safe_psql('postgres', "CLUSTER $t USING ${t}_id");
	is($rr->query_safe("SELECT big = @{[val(2)]} FROM $t WHERE id = 2"),
		't', "$am: old snapshot reads the recently dead row after CLUSTER");
	$rr->query_safe('COMMIT');
	$rr->quit;
	vacuum_both($t);
	my $cl = state_of($t);
	note "$am CLUSTER state: $cl";
	like(
		$cl,
		qr/^1\/1\/true 3\/3\/true 4\/4\/true\n/,
		"$am: rows after CLUSTER and VACUUM");
	$node->safe_psql('postgres', "SELECT 1");
}

# CLUSTER: FLUX keeps exactly what heap keeps.
is(state_of('cl_flux'), state_of('cl_heap'),
	'CLUSTER of a recently dead row: FLUX TOAST state equals heap');

# --- Logical decoding of a DELETE, REPLICA IDENTITY FULL. ---
my %out;
foreach my $am ('heap', 'flux')
{
	my $t = "ld_$am";
	make($t, $am);
	$node->safe_psql('postgres', "ALTER TABLE $t REPLICA IDENTITY FULL");
	$node->safe_psql('postgres',
		"SELECT pg_create_logical_replication_slot('s_$am', 'test_decoding')"
	);
	$node->safe_psql('postgres', "DELETE FROM $t WHERE id IN (2, 3)");
	$out{$am} = $node->safe_psql(
		'postgres', qq{
SELECT string_agg(data, E'\\n') FROM pg_logical_slot_get_changes('s_$am',
	NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1')});
	$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_$am')");
	$out{$am} =~ s/ld_$am/ld_AM/g;
	vacuum_both($t);
	is( state_of($t),
		"1/1/true 4/4/true\n2\nt",
		"$am: decoded DELETE left two values");
}
like($out{heap}, qr/DELETE: id\[integer\]:2/, 'heap DELETE decoded');
is($out{flux}, $out{heap},
	'logical decoding of a DELETE with TOAST values: FLUX output equals heap'
);

$node->stop;

# --- A DELETE rolled back while its revert is pending, then helped. ---
# The rollback goes to an undo apply worker (undo_rollback_overflow_size =
# 0), held at an injection point; an UPDATE of the row reverts the aborted
# DELETE itself first.  The DELETE's TOAST deletes rolled back with it.
SKIP:
{
	skip 'injection points not supported by this build', 3
	  unless ($ENV{enable_injection_points} // '') eq 'yes';
	my $h = PostgreSQL::Test::Cluster->new('flux_toast_helped');
	$h->init;
	$h->append_conf(
		'postgresql.conf', qq{
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
});
	$h->start;
	skip 'injection_points not installed', 3
	  unless $h->check_extension('injection_points');
	$node = $h;
	$h->safe_psql(
		'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION injection_points;
CREATE FUNCTION release_one() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
				 WHERE wait_event = 'pbu-undo-worker-before-apply') THEN
	RETURN true;
  END IF;
  BEGIN
	PERFORM injection_points_wakeup('pbu-undo-worker-before-apply');
  EXCEPTION WHEN OTHERS THEN
	NULL;
  END;
  RETURN false;
END $$;
});
	make('hp', 'flux');
	$h->safe_psql('postgres',
		"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')"
	);
	$h->safe_psql('postgres', 'BEGIN; DELETE FROM hp WHERE id = 1; ROLLBACK');
	$h->poll_query_until(
		'postgres', q{
SELECT count(*) > 0 FROM pg_stat_activity
WHERE wait_event = 'pbu-undo-worker-before-apply'}
	) or die 'the rollback did not go to a worker';
	$h->safe_psql('postgres', 'UPDATE hp SET k = 10 WHERE id = 1');
	is( $h->safe_psql(
			'postgres', "SELECT k, big = @{[val(1)]} FROM hp WHERE id = 1"),
		'10|t',
		'flux: the helped row has its value');
	$h->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
	$h->poll_query_until('postgres', 'SELECT release_one()')
	  or die 'worker not released';
	vacuum_both('hp');
	is( state_of('hp'),
		"1/10/true 2/2/true 3/3/true 4/4/true\n4\nt",
		'flux: helped revert of a DELETE keeps every value');
	is($h->safe_psql('postgres', 'SELECT count(*) FROM hp'),
		'4', 'flux: no row lost');
	$h->stop;
}

done_testing();
