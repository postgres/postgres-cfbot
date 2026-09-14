
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Row-lock conflict probes must never miss a lock, whatever lets them skip
# holders.
#
# A conflict probe skips the holders whose filters say they hold no lock in
# the probed tuple's block.  The filters are hashed and saturate, so they
# may say "maybe" wrongly but must never say "no" for a lock that exists.
# One session locks a row on each of many more blocks than the filter has
# bits, in two relations, so that the probes below hit every collision; a
# second session must then find each locked row locked and each other row
# free.  Then the same through a rolled-back savepoint, a prepared
# transaction, and a restart with the prepared transaction still there.
#
# A holder keeps its first few locked rows in records of its own slot, read
# without any lock, and the rest in trees; a row's locks are in exactly one
# of the two places.  So the probes also run on transactions that lock
# 1, 8 and 9 rows (the records hold 8), with a savepoint level stacked on
# one of them, prepared and restored, after the holder's earlier
# transactions (whose emptied tables it may keep), and under concurrent FK
# inserts and cascading parent deletes, which must leave no orphan.
#
# Past the records a slot keeps a relation's locks in a hash table of its
# own that grows: so also many holders of the same rows and of rows on
# different blocks, released one at a time; a transaction that locks 1600
# rows, with a savepoint that upgrades 500 of them and rolls back, prepared
# and restored; a savepoint across the boundary between records and table;
# a relation truncated while the transaction holds locks in it and in
# another; and FK inserts of 25 rows per transaction.
#
# Heap is the control.
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
max_prepared_transactions = 2
));
$node->start;

# 600 blocks per relation at 20 rows per block (fillfactor 10).
my $nblocks = 600;

# Rows locked: offset 1 of every even block.  Not locked: offset 1 of every
# odd block, and offset 2 of every even block (same block as a lock).
sub probe_counts
{
	my ($am) = @_;
	my %got;
	foreach my $rel ("a_$am", "b_$am")
	{
		my ($stdout, $stderr);
		$node->psql(
			'postgres', qq{
SET client_min_messages = notice;
DO \$\$
DECLARE
  r record; locked int := 0; free_odd int := 0; free_even int := 0;
BEGIN
  FOR r IN SELECT ctid, (ctid::text::point)[0]::int AS blk,
                  (ctid::text::point)[1]::int AS off
           FROM $rel WHERE (ctid::text::point)[1] IN (1, 2) LOOP
    BEGIN
      PERFORM 1 FROM $rel WHERE ctid = r.ctid FOR UPDATE NOWAIT;
      IF r.blk % 2 = 1 THEN free_odd := free_odd + 1;
      ELSIF r.off = 2 THEN free_even := free_even + 1;
      END IF;
    EXCEPTION WHEN lock_not_available THEN
      IF r.blk % 2 = 0 AND r.off = 1 THEN locked := locked + 1; END IF;
    END;
  END LOOP;
  RAISE NOTICE 'counts % % %', locked, free_odd, free_even;
END \$\$;
},
			stdout => \$stdout,
			stderr => \$stderr);
		$stderr =~ /counts (\d+) (\d+) (\d+)/
		  or die "no counts for $rel: $stderr";
		$got{$rel} = "$1 $2 $3";
	}
	return \%got;
}

my $half = $nblocks / 2;
foreach my $am ('heap', 'flux')
{
	foreach my $rel ("a_$am", "b_$am")
	{
		$node->safe_psql(
			'postgres', qq{
CREATE TABLE $rel (id int PRIMARY KEY, pad text) USING $am WITH (fillfactor = 10);
INSERT INTO $rel SELECT g, repeat('x', 300) FROM generate_series(1, $nblocks * 20) g;
});
	}
	my $nblk = $node->safe_psql('postgres',
		"SELECT count(DISTINCT (ctid::text::point)[0]) FROM a_$am");
	cmp_ok($nblk, '>=', 300,
		"$am: relation spans many more blocks than the filter has bits");
	my $even = $node->safe_psql('postgres',
		"SELECT count(DISTINCT (ctid::text::point)[0]) FROM a_$am WHERE (ctid::text::point)[0]::int % 2 = 0"
	);
	# offsets 1 and 2 of each odd block are free
	my $odd = 2 * ($nblk - $even);
	my $lock_sql = join(
		'',
		map {
			"SELECT count(*) FROM (SELECT 1 FROM $_ WHERE (ctid::text::point)[1] = 1 "
			  . "AND (ctid::text::point)[0]::int % 2 = 0 FOR KEY SHARE) s;"
		} ("a_$am", "b_$am"));

	# Plain transaction.
	my $h = $node->background_psql('postgres');
	$h->query_safe('BEGIN');
	$h->query_safe($lock_sql);
	my $c = probe_counts($am, 'plain');
	is( $c->{"a_$am"},
		"$even $odd $even",
		"$am a: every lock found, no false lock (plain)");
	is( $c->{"b_$am"},
		"$even $odd $even",
		"$am b: every lock found, no false lock (plain)");
	$h->query_safe('COMMIT');
	$c = probe_counts($am, 'after commit');
	is($c->{"a_$am"}, "0 $odd $even", "$am a: locks gone after commit");

	# Locks taken in a rolled-back savepoint are dead; outer ones stay.
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM a_$am WHERE (ctid::text::point)[1] = 1 AND (ctid::text::point)[0]::int % 2 = 0 FOR KEY SHARE) s"
	);
	$h->query_safe('SAVEPOINT s');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM b_$am WHERE (ctid::text::point)[1] = 1 AND (ctid::text::point)[0]::int % 2 = 0 FOR UPDATE) s"
	);
	$h->query_safe('ROLLBACK TO s');
	$c = probe_counts($am, 'savepoint');
	is( $c->{"a_$am"},
		"$even $odd $even",
		"$am a: outer locks kept after ROLLBACK TO");
	is($c->{"b_$am"}, "0 $odd $even",
		"$am b: savepoint locks dead after ROLLBACK TO");
	$h->query_safe('ROLLBACK');

	# Prepared, and across a restart.
	$h->query_safe('BEGIN');
	$h->query_safe($lock_sql);
	$h->query_safe("PREPARE TRANSACTION 'p_$am'");
	$h->quit;
	$c = probe_counts($am, 'prepared');
	is($c->{"a_$am"}, "$even $odd $even", "$am a: prepared locks found");
	is($c->{"b_$am"}, "$even $odd $even", "$am b: prepared locks found");
	$node->restart;
	$c = probe_counts($am, 'prepared after restart');
	is( $c->{"a_$am"},
		"$even $odd $even",
		"$am a: prepared locks found after restart");
	is( $c->{"b_$am"},
		"$even $odd $even",
		"$am b: prepared locks found after restart");
	$node->safe_psql('postgres', "COMMIT PREPARED 'p_$am'");
	$c = probe_counts($am, 'after commit prepared');
	is($c->{"b_$am"}, "0 $odd $even",
		"$am b: locks gone after COMMIT PREPARED");
}

# Few rows: the first SLOG_TL_NINLINE (8) TIDs of a transaction are kept
# inline, the rest in trees.  Lock k rows of c_$am, then probe all of them.
sub probe_rows
{
	my ($am, $lo, $hi, $mode, $rel) = @_;
	my ($stdout, $stderr);
	$rel //= "c_$am";
	$node->psql(
		'postgres', qq{
SET client_min_messages = notice;
DO \$\$
DECLARE i int; blocked int := 0; free int := 0;
BEGIN
  FOR i IN $lo..$hi LOOP
    BEGIN
      PERFORM 1 FROM $rel WHERE id = i FOR $mode NOWAIT;
      free := free + 1;
    EXCEPTION WHEN lock_not_available THEN blocked := blocked + 1;
    END;
  END LOOP;
  RAISE NOTICE 'rows % %', blocked, free;
END \$\$;
},
		stdout => \$stdout,
		stderr => \$stderr);
	$stderr =~ /rows (\d+) (\d+)/ or die "no rows for $rel: $stderr";
	return "$1 $2";
}

foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE c_$am (id int PRIMARY KEY, v int) USING $am;
INSERT INTO c_$am SELECT g, g FROM generate_series(1, 40) g;
});
	my $h = $node->background_psql('postgres');
	foreach my $k (1, 8, 9, 12)
	{
		$h->query_safe('BEGIN');
		$h->query_safe(
			"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id <= $k ORDER BY id FOR KEY SHARE) s"
		);
		is( probe_rows($am, 1, 20, 'UPDATE'),
			"$k " . (20 - $k),
			"$am: $k rows locked, all found, no false lock");
		is(probe_rows($am, 1, 20, 'NO KEY UPDATE'),
			"0 20", "$am: $k KEY SHARE rows do not block NO KEY UPDATE");
		$h->query_safe('COMMIT');
		is(probe_rows($am, 1, 20, 'UPDATE'),
			"0 20", "$am: $k rows free after commit");
	}

	# A savepoint level on an inline row and on a row in a tree: after
	# ROLLBACK TO only the outer KEY SHARE is left.
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id <= 10 ORDER BY id FOR KEY SHARE) s"
	);
	$h->query_safe('SAVEPOINT s');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id IN (2, 10) FOR UPDATE) s"
	);
	is(probe_rows($am, 1, 10, 'NO KEY UPDATE'),
		"2 8",
		"$am: savepoint FOR UPDATE found on an inline row and a tree row");
	$h->query_safe('ROLLBACK TO s');
	is(probe_rows($am, 1, 10, 'NO KEY UPDATE'),
		"0 10", "$am: savepoint levels dead after ROLLBACK TO");
	is(probe_rows($am, 1, 10, 'UPDATE'),
		"10 0", "$am: outer KEY SHARE kept after ROLLBACK TO");
	$h->query_safe('ROLLBACK');

	# Inline-only and mixed prepared transactions, across a restart.
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id <= 3 FOR UPDATE) s"
	);
	$h->query_safe("PREPARE TRANSACTION 'q1_$am'");
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id BETWEEN 11 AND 30 FOR KEY SHARE) s"
	);
	$h->query_safe("PREPARE TRANSACTION 'q2_$am'");
	$h->quit;
	is(probe_rows($am, 1, 40, 'UPDATE'),
		"23 17", "$am: prepared locks found (inline only, and mixed)");
	$node->restart;
	is(probe_rows($am, 1, 40, 'UPDATE'),
		"23 17", "$am: prepared locks found after restart");
	is(probe_rows($am, 11, 30, 'NO KEY UPDATE'),
		"0 20", "$am: restored KEY SHARE keeps its strength");
	$node->safe_psql('postgres', "COMMIT PREPARED 'q1_$am'");
	is(probe_rows($am, 1, 40, 'UPDATE'),
		"20 20", "$am: one prepared transaction's locks gone");
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'q2_$am'");
	is(probe_rows($am, 1, 40, 'UPDATE'),
		"0 40", "$am: all prepared locks gone");
}

# A backend may keep its emptied trees for its next transactions; a tree
# that went to a prepared transaction must not be reused.  One session locks
# 10 rows (two of them in a tree) and commits, so that it may keep that tree;
# its next transaction locks 10 rows, the last two in a tree again, and
# prepares; its third locks 10 other rows and commits.  The prepared rows
# must stay locked throughout and until ROLLBACK PREPARED.
foreach my $am ('heap', 'flux')
{
	my $h = $node->background_psql('postgres');
	my $lock = sub {
		my ($lo, $hi) = @_;
		$h->query_safe('BEGIN');
		$h->query_safe(
			"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id BETWEEN $lo AND $hi ORDER BY id FOR UPDATE) s"
		);
	};
	$lock->(1, 10);
	$h->query_safe('COMMIT');
	$lock->(11, 20);
	$h->query_safe("PREPARE TRANSACTION 'r_$am'");
	is(probe_rows($am, 11, 20, 'UPDATE'),
		"10 0", "$am: prepared rows locked");
	$lock->(21, 30);
	is(probe_rows($am, 11, 30, 'UPDATE'),
		"20 0", "$am: prepared rows and the next transaction's rows locked");
	$h->query_safe('COMMIT');
	is(probe_rows($am, 11, 30, 'UPDATE'),
		"10 10", "$am: prepared rows still locked after the next commit");
	$lock->(31, 40);
	$h->query_safe('ROLLBACK');
	is(probe_rows($am, 11, 40, 'UPDATE'),
		"10 20", "$am: prepared rows still locked after a rollback");
	$h->quit;
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'r_$am'");
	is(probe_rows($am, 1, 40, 'UPDATE'), "0 40", "$am: all free at the end");
}

# Many holders.  Five sessions lock the same 20 rows (8 in records, 12 in a
# table each); they stay locked until the last one commits.  Then five
# sessions lock 12 rows each on blocks of their own; committing one frees
# exactly its rows.
foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE d_$am (id int PRIMARY KEY, v int) USING $am;
INSERT INTO d_$am SELECT g, g FROM generate_series(1, 3000) g;
});
	my $nblk = $node->safe_psql('postgres',
		"SELECT count(DISTINCT (ctid::text::point)[0]) FROM d_$am WHERE id IN (1, 501, 1001, 1501, 2001)"
	);
	is($nblk, '5', "$am: the five ranges are on five blocks");

	my @h = map { $node->background_psql('postgres') } 1 .. 5;
	foreach my $s (@h)
	{
		$s->query_safe('BEGIN');
		$s->query_safe(
			"SELECT count(*) FROM (SELECT 1 FROM d_$am WHERE id <= 20 ORDER BY id FOR KEY SHARE) s"
		);
	}
	is(probe_rows($am, 1, 20, 'UPDATE', "d_$am"),
		"20 0", "$am: five holders of the same rows: all found");
	is(probe_rows($am, 1, 20, 'NO KEY UPDATE', "d_$am"),
		"0 20", "$am: five KEY SHARE holders do not block NO KEY UPDATE");
	foreach my $k (0 .. 3)
	{
		$h[$k]->query_safe('COMMIT');
		is( probe_rows($am, 1, 20, 'UPDATE', "d_$am"),
			"20 0",
			"$am: rows still locked after "
			  . ($k + 1)
			  . " of 5 holders commit");
	}
	$h[4]->query_safe('COMMIT');
	is(probe_rows($am, 1, 20, 'UPDATE', "d_$am"),
		"0 20", "$am: rows free after the last holder commits");

	foreach my $k (0 .. 4)
	{
		my $lo = $k * 500 + 1;
		my $hi = $lo + 11;
		$h[$k]->query_safe('BEGIN');
		$h[$k]->query_safe(
			"SELECT count(*) FROM (SELECT 1 FROM d_$am WHERE id BETWEEN $lo AND $hi ORDER BY id FOR UPDATE) s"
		);
	}
	$h[2]->query_safe('COMMIT');
	my @got =
	  map { probe_rows($am, $_ * 500 + 1, $_ * 500 + 12, 'UPDATE', "d_$am") }
	  0 .. 4;
	is( "@got",
		"12 0 12 0 0 12 12 0 12 0",
		"$am: holders on different blocks: one commit frees exactly its rows"
	);
	$h[$_]->query_safe('ROLLBACK') foreach (0, 1, 3, 4);
	$_->quit foreach @h;
	is(probe_rows($am, 1, 2012, 'UPDATE', "d_$am"),
		"0 2012", "$am: all free after the holders end");
}

# More than a thousand rows, a savepoint that upgrades some and rolls back,
# then more rows, prepared and restored across a restart.
foreach my $am ('heap', 'flux')
{
	my $h = $node->background_psql('postgres');
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM d_$am WHERE id <= 1500 FOR KEY SHARE) s"
	);
	is(probe_rows($am, 1, 1500, 'UPDATE', "d_$am"),
		"1500 0", "$am: 1500 rows locked, all found");
	is(probe_rows($am, 1, 1500, 'NO KEY UPDATE', "d_$am"),
		"0 1500", "$am: 1500 KEY SHARE rows do not block NO KEY UPDATE");
	$h->query_safe('SAVEPOINT s');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM d_$am WHERE id > 1000 AND id <= 1500 FOR UPDATE) s"
	);
	is(probe_rows($am, 1, 1500, 'NO KEY UPDATE', "d_$am"),
		"500 1000", "$am: 500 rows upgraded in a savepoint");
	$h->query_safe('ROLLBACK TO s');
	is(probe_rows($am, 1, 1500, 'NO KEY UPDATE', "d_$am"),
		"0 1500", "$am: upgrades dead after ROLLBACK TO");
	is(probe_rows($am, 1, 1500, 'UPDATE', "d_$am"),
		"1500 0", "$am: 1500 outer locks kept after ROLLBACK TO");
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM d_$am WHERE id BETWEEN 1501 AND 1600 FOR KEY SHARE) s"
	);
	$h->query_safe("PREPARE TRANSACTION 'big_$am'");
	$h->quit;
	is(probe_rows($am, 1, 1700, 'UPDATE', "d_$am"),
		"1600 100", "$am: 1600 prepared locks found");
	$node->restart;
	is(probe_rows($am, 1, 1700, 'UPDATE', "d_$am"),
		"1600 100", "$am: 1600 prepared locks found after restart");
	is(probe_rows($am, 1, 1700, 'NO KEY UPDATE', "d_$am"),
		"0 1700", "$am: 1600 restored locks keep their strength");
	$node->safe_psql('postgres', "COMMIT PREPARED 'big_$am'");
	is(probe_rows($am, 1, 1700, 'UPDATE', "d_$am"),
		"0 1700", "$am: 1600 locks gone after COMMIT PREPARED");
}

# A savepoint across the boundary between records and table: 6 rows, then
# in a savepoint 6 more (2 in records, 4 in the table) and an upgrade of an
# outer one; after ROLLBACK TO, the same rows locked again.
foreach my $am ('heap', 'flux')
{
	my $h = $node->background_psql('postgres');
	$h->query_safe('BEGIN');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id <= 6 ORDER BY id FOR KEY SHARE) s"
	);
	$h->query_safe('SAVEPOINT s');
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id BETWEEN 7 AND 12 OR id = 3 ORDER BY id FOR UPDATE) s"
	);
	is(probe_rows($am, 1, 12, 'UPDATE'),
		"12 0", "$am: savepoint across the boundary: all 12 locked");
	is(probe_rows($am, 1, 12, 'NO KEY UPDATE'),
		"7 5", "$am: savepoint across the boundary: its 7 FOR UPDATE found");
	$h->query_safe('ROLLBACK TO s');
	is(probe_rows($am, 1, 12, 'NO KEY UPDATE'),
		"0 12", "$am: savepoint across the boundary: dead after ROLLBACK TO");
	is(probe_rows($am, 1, 12, 'UPDATE'),
		"6 6", "$am: savepoint across the boundary: outer 6 kept");
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id BETWEEN 7 AND 12 ORDER BY id FOR KEY SHARE) s"
	);
	is(probe_rows($am, 1, 12, 'UPDATE'),
		"12 0", "$am: rows locked again after ROLLBACK TO");
	$h->query_safe('COMMIT');
	is(probe_rows($am, 1, 12, 'UPDATE'), "0 12",
		"$am: all free after commit");
	$h->quit;
}

# A relation truncated while the transaction holds locks in it (records and
# table) and in another (records and table): the other's locks stay, before
# and after PREPARE and a restart.
foreach my $am ('heap', 'flux')
{
	my $h = $node->background_psql('postgres');
	$h->query_safe('BEGIN');
	$h->query_safe(
		"CREATE TABLE f_$am (id int PRIMARY KEY) USING $am; INSERT INTO f_$am SELECT generate_series(1, 40)"
	);
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id <= 4 ORDER BY id FOR UPDATE) s"
	);
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM f_$am WHERE id <= 20 ORDER BY id FOR UPDATE) s"
	);
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM c_$am WHERE id BETWEEN 5 AND 20 ORDER BY id FOR UPDATE) s"
	);
	$h->query_safe("TRUNCATE f_$am");
	$h->query_safe("INSERT INTO f_$am SELECT generate_series(1, 40)");
	$h->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM f_$am WHERE id <= 10 ORDER BY id FOR KEY SHARE) s"
	);
	is(probe_rows($am, 1, 40, 'UPDATE'),
		"20 20", "$am: locks in another relation kept across TRUNCATE");
	$h->query_safe("PREPARE TRANSACTION 'tr_$am'");
	$h->quit;
	is(probe_rows($am, 1, 40, 'UPDATE'), "20 20", "$am: they are prepared");
	$node->restart;
	is(probe_rows($am, 1, 40, 'UPDATE'), "20 20", "$am: they are restored");
	$node->safe_psql('postgres', "COMMIT PREPARED 'tr_$am'");
	is(probe_rows($am, 1, 40, 'UPDATE'), "0 40", "$am: all free at the end");
	is(probe_rows($am, 1, 40, 'UPDATE', "f_$am"),
		"0 40", "$am: truncated relation's rows free at the end");
}


# Concurrency: FK inserts take KEY SHARE on the parent while other sessions
# delete parents, cascading.  A parent DELETE that missed a KEY SHARE lock
# would let the child commit without its parent.
foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE p_$am (id int PRIMARY KEY) USING $am;
INSERT INTO p_$am SELECT generate_series(1, 3000);
CREATE TABLE k_$am (pid int REFERENCES p_$am ON DELETE CASCADE) USING $am;
CREATE FUNCTION ins_$am(int) RETURNS void LANGUAGE plpgsql AS \$\$
BEGIN INSERT INTO k_$am VALUES (\$1);
EXCEPTION WHEN foreign_key_violation THEN NULL; END \$\$;
});
	$node->pgbench(
		'--no-vacuum --client=8 --jobs=8 --transactions=1500',
		0,
		[qr{processed: 12000/12000}],
		[qr{^$}],
		"$am: concurrent FK inserts and cascading parent deletes",
		{
			"135_ins_$am" =>
			  "\\set id random(1, 3000)\nSELECT ins_$am(:id);\n",
			"135_del_$am\@1" =>
			  "\\set id random(1, 3000)\nDELETE FROM p_$am WHERE id = :id;\n",
			"135_ins2_$am\@3" =>
			  "\\set id random(1, 3000)\nBEGIN;\nSELECT ins_$am(:id);\nSELECT ins_$am(:id + 1);\nSELECT ins_$am(:id + 2);\nEND;\n",
			"135_insm_$am\@2" =>
			  "\\set id random(1, 2976)\nSELECT count(ins_$am(g)) FROM generate_series(:id, :id + 24) g;\n"
		});
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM k_$am k WHERE NOT EXISTS (SELECT 1 FROM p_$am p WHERE p.id = k.pid)"
		),
		'0',
		"$am: no orphaned child row");
	cmp_ok($node->safe_psql('postgres', "SELECT count(*) FROM p_$am"),
		'<', 3000, "$am: some parents were deleted");
}

$node->stop;
done_testing();
