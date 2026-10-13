
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX and heap delete a row's external TOAST values in the deleting
# transaction.  TOAST VACUUM reclaims those values and values whose inserter
# aborted.  Both AMs must end with the same rows and with no TOAST chunk
# that no row references.
#
# Each case runs on a heap and a FLUX twin and checks that VACUUM succeeds
# (twice), the rows and their values, and, after VACUUM and a VACUUM of the
# TOAST table, that every TOAST chunk left belongs to a live row.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_vacuum_toast');
$node->init;
my $injection = ($ENV{enable_injection_points} // '') eq 'yes';
$node->append_conf('postgresql.conf',
	"autovacuum = off\n"
	  . ($injection ? "shared_preload_libraries = 'injection_points'\n" : '')
);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pageinspect');
$injection = 0 unless $node->check_extension('injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points')
  if $injection;

# Row id's value: about 10 kB, stored uncompressed out of line.
my $val = sub { my ($id) = @_; return "($id || ':' || repeat('x', 10000))" };

sub make
{
	my ($t, $am, $ids) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, k int, big text) USING $am
	WITH (autovacuum_enabled = off);
ALTER TABLE $t ALTER big SET STORAGE EXTERNAL;
CREATE UNIQUE INDEX ${t}_id ON $t (id);
CREATE INDEX ${t}_k ON $t (k);
INSERT INTO $t SELECT g, g, @{[$val->('g')]} FROM unnest('{$ids}'::int[]) g;
});
}

sub toast_of
{
	my ($t) = @_;
	return $node->safe_psql('postgres',
		"SELECT reltoastrelid::regclass FROM pg_class WHERE oid = '$t'::regclass"
	);
}

# VACUUM; returns '' on success, else the error.
sub vacuum
{
	my ($what) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', "VACUUM $what");
	return $rc == 0 ? '' : $err;
}

# Rows, each with its value intact, and TOAST state: values an MVCC
# snapshot sees, and whether every chunk on the TOAST pages is one of them.
sub state_of
{
	my ($t) = @_;
	my $toast = toast_of($t);
	return $node->safe_psql(
		'postgres', qq{
SELECT string_agg(id || '/' || k || '/' ||
				  (big = @{[$val->('id')]})::text, ' ' ORDER BY id)
FROM $t;
SELECT count(DISTINCT chunk_id) FROM $toast;
SELECT (SELECT count(*)
		FROM generate_series(0, pg_relation_size('$toast') /
			 current_setting('block_size')::int - 1) b,
			 heap_page_items(get_raw_page('$toast', b::int))
		WHERE lp_flags <> 0) = (SELECT count(*) FROM $toast)});
}

# name, the change to make on table T (rows 1-4 exist), the rows expected.
my @cases = (
	[
		'committed DELETE',
		'DELETE FROM T WHERE id = 4',
		'1/1/true 2/2/true 3/3/true'
	],
	[
		'aborted INSERT',
		"BEGIN; INSERT INTO T SELECT g, g, @{[$val->('g')]}
			FROM generate_series(11, 13) g; ROLLBACK",
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],
	[
		'INSERT rolled back to a savepoint in a committed transaction',
		"BEGIN; INSERT INTO T VALUES (21, 21, @{[$val->(21)]});
		 SAVEPOINT s;
		 INSERT INTO T SELECT g, g, @{[$val->('g')]}
			FROM generate_series(22, 23) g;
		 ROLLBACK TO s; COMMIT",
		'1/1/true 2/2/true 3/3/true 4/4/true 21/21/true'
	],
	[
		'aborted in-place UPDATE of the TOASTed column',
		"BEGIN; UPDATE T SET big = 'new' || repeat('y', 10000) WHERE id = 1;
		 ROLLBACK",
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],
	# A hash index has no delete-marking, so FLUX moves a row whose key
	# changes to a new TID (delete + insert), as heap does.
	[
		'aborted out-of-place UPDATE (key change, new value)',
		"CREATE INDEX ON T USING hash (k);
		 BEGIN; UPDATE T SET k = 102, big = 'new' || repeat('y', 10000)
			WHERE id = 2; ROLLBACK",
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],
	[
		'aborted out-of-place UPDATE (key change, value kept)',
		'CREATE INDEX ON T USING hash (k);
		 BEGIN; UPDATE T SET k = 103 WHERE id = 3; ROLLBACK',
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],
	[
		'committed out-of-place UPDATE (key change, value kept)',
		"CREATE INDEX ON T USING hash (k);
		 UPDATE T SET k = 103 WHERE id = 3;
		 DO \$\$ BEGIN IF (SELECT ctid FROM T WHERE id = 3) = '(0,3)'
		 THEN RAISE 'row did not move'; END IF; END \$\$",
		'1/1/true 2/2/true 3/103/true 4/4/true'
	],
	[
		'DELETE rolled back',
		'BEGIN; DELETE FROM T WHERE id IN (1, 2); ROLLBACK',
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],
	[
		'INSERT and DELETE in one committed transaction',
		"BEGIN; INSERT INTO T VALUES (31, 31, @{[$val->(31)]});
		 DELETE FROM T WHERE id = 31; COMMIT",
		'1/1/true 2/2/true 3/3/true 4/4/true'
	],);

my $n = 0;
foreach my $case (@cases)
{
	my ($name, $sql, $rows) = @$case;
	$n++;
	foreach my $am ('heap', 'flux')
	{
		my $t = "c${n}_$am";
		make($t, $am, '1,2,3,4');
		(my $s = $sql) =~ s/\bT\b/$t/g;
		$node->safe_psql('postgres', $s);

		is(vacuum($t), '', "$am, $name: VACUUM succeeds");
		is(vacuum(toast_of($t)), '', "$am, $name: VACUUM of the TOAST table");
		my $nrows = () = $rows =~ /\//g;
		is( state_of($t),
			"$rows\n" . ($nrows / 2) . "\nt",
			"$am, $name: rows and TOAST after VACUUM");
		is(vacuum($t), '', "$am, $name: a second VACUUM succeeds");
	}
}

# A VACUUM that cannot compact a page (a cursor pins it, and a
# non-aggressive VACUUM does not wait) removes none of its rows, so it must
# not delete their TOAST values either: the next VACUUM would find them
# deleted but, while the cursor's transaction holds the horizon, not yet
# removed.
foreach my $am ('heap', 'flux')
{
	my $t = "pin_$am";
	make($t, $am, '1,2,3,4');
	$node->safe_psql('postgres', "DELETE FROM $t WHERE id = 2");
	my $cur = $node->background_psql('postgres');
	$cur->query_safe("BEGIN; DECLARE c CURSOR FOR SELECT id FROM $t");
	$cur->query_safe('FETCH 1 FROM c');
	is(vacuum($t), '', "$am, page pinned: VACUUM succeeds");
	is(vacuum($t), '', "$am, page pinned: a second VACUUM succeeds");
	$cur->query_safe('COMMIT');
	$cur->quit;
	is(vacuum($t), '', "$am, unpinned: VACUUM succeeds");
	is(vacuum(toast_of($t)), '', "$am, unpinned: VACUUM of the TOAST table");
	is( state_of($t),
		"1/1/true 3/3/true 4/4/true\n3\nt",
		"$am, unpinned: rows and TOAST after VACUUM");
}

# Opportunistic pruning reclaims a committed-deleted row's storage before
# VACUUM sees it.  Its TOAST values must not outlive it.
foreach my $am ('heap', 'flux')
{
	my $t = "prune_$am";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, big text, pad text) USING $am
	WITH (autovacuum_enabled = off);
ALTER TABLE $t ALTER big SET STORAGE EXTERNAL;
INSERT INTO $t SELECT g, @{[$val->('g')]}, repeat('p', 200)
	FROM generate_series(1, 40) g;
DELETE FROM $t WHERE id % 2 = 0;
});
	# The page is nearly full, so a scan prunes it.
	$node->safe_psql('postgres', "SELECT count(*) FROM $t");
	$node->safe_psql('postgres', "SELECT count(*) FROM $t");
	is(vacuum($t), '', "$am, pruned: VACUUM succeeds");
	is(vacuum(toast_of($t)), '', "$am, pruned: VACUUM of the TOAST table");
	is( $node->safe_psql(
			'postgres', qq{
SELECT (SELECT count(*) FROM $t),
	(SELECT count(DISTINCT chunk_id) FROM @{[toast_of($t)]})}),
		'20|20',
		"$am, pruned: one TOAST value per live row");
}

# A crash after VACUUM removed rows, before it committed.  The page changes
# are not transactional, so the rows are gone for good; their TOAST values
# must not stay behind.  The FLUX VACUUM stops just before truncating the
# table, after it has compacted every page.  Heap has no such stop; its twin
# crashes right after the DELETE.
SKIP:
{
	skip 'injection points not supported by this build', 6
	  unless $injection;
	foreach my $am ('heap', 'flux')
	{
		my $t = "crash_$am";
		$node->safe_psql(
			'postgres', qq{
CREATE TABLE $t (id int, big text) USING $am WITH (autovacuum_enabled = off);
ALTER TABLE $t ALTER big SET STORAGE EXTERNAL;
INSERT INTO $t SELECT g, g || repeat('v', 2100) FROM generate_series(1, 8000) g;
CREATE INDEX ${t}_id ON $t (id);
DELETE FROM $t WHERE id > 100;
});
		my $vac;
		if ($am eq 'flux')
		{
			$vac = $node->background_psql('postgres', on_error_stop => 0);
			$vac->query_safe(
				"SELECT injection_points_set_local();
				 SELECT injection_points_attach('flux-vacuum-before-truncate', 'wait')"
			);
			$vac->query_until(qr/start/, "\\echo start\nVACUUM $t;\n");
			$node->wait_for_event('client backend',
				'flux-vacuum-before-truncate');
		}
		$node->stop('immediate');
		eval { $vac->quit } if $vac;
		$node->start;
		is(vacuum($t), '', "$am, after a crash: VACUUM succeeds");
		is(vacuum(toast_of($t)),
			'', "$am, after a crash: VACUUM of the TOAST table");
		is( $node->safe_psql(
				'postgres', qq{
SELECT (SELECT count(*) FROM $t),
	(SELECT count(DISTINCT chunk_id) FROM @{[toast_of($t)]})}),
			'100|100',
			"$am, after a crash: one TOAST value per live row");
	}
}

# A speculative insertion that loses to a concurrent INSERT is killed with
# its TOAST values (heap_abort_speculative); its transaction commits.
SKIP:
{
	skip 'injection points not supported by this build', 8
	  unless $injection;
	foreach my $am ('heap', 'flux')
	{
		my $t = "spec_$am";
		make($t, $am, '1');
		my $s1 = $node->background_psql('postgres');
		$s1->query_safe(
			"SELECT injection_points_set_local();
			 SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait')"
		);
		$s1->query_until(
			qr/start/, qq{\\echo start
INSERT INTO $t VALUES (5, 5, @{[$val->(5)]}) ON CONFLICT (id) DO NOTHING;
});
		$node->wait_for_event('client backend',
			'exec-insert-before-insert-speculative');
		$node->safe_psql('postgres',
			"INSERT INTO $t VALUES (5, 5, @{[$val->(5)]})");
		$node->safe_psql('postgres',
			"SELECT injection_points_wakeup('exec-insert-before-insert-speculative')"
		);
		$s1->query_safe(
			"SELECT injection_points_detach('exec-insert-before-insert-speculative')"
		);
		$s1->quit;

		is(vacuum($t), '', "$am, aborted speculative insertion: VACUUM");
		is(vacuum(toast_of($t)),
			'',
			"$am, aborted speculative insertion: VACUUM of the TOAST table");
		is( state_of($t),
			"1/1/true 5/5/true\n2\nt",
			"$am, aborted speculative insertion: rows and TOAST");
		is(vacuum($t), '',
			"$am, aborted speculative insertion: a second VACUUM");
	}
}

$node->stop;
done_testing();
