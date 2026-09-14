
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# REPACK (CONCURRENTLY) of a FLUX table across a crash, on a standby, and at
# wal_level = replica, each next to a heap twin that runs the same statements.
#
# Crash: the server is stopped in immediate mode while REPACK
#  - slot: has its decoding worker create the replication slot
#    (replication-slot-create-begin), before the initial snapshot;
#  - copied: has copied the table and built the new indexes, and waits at
#    repack-concurrently-before-lock while other sessions change the table;
#  - caught_up: has replayed those changes onto the new table (FLUX changes
#    the new rows in place, with undo, as REPACK's transaction) and waits for
#    its AccessExclusiveLock;
#  - commit: has flushed its commit record (commit-before-clog-update);
#  - committed: has returned.
# After recovery the table and its indexes must hold exactly the rows that
# were committed before the crash, in the new file only if REPACK's commit
# record was written, and nothing of the transient table may be left: no
# catalog entry, no file, no slot, no undo holding WAL back.
#
# Standby: a FLUX REPACK (CONCURRENTLY) is replayed.  A repeatable read
# transaction on the standby that read the table before the swap holds the
# replay back and keeps reading the old rows; once it ends, the standby has
# the primary's new file, rows and intact indexes.
#
# wal_level = replica: REPACK enables logical decoding itself while a
# transaction that wrote the table's FLUX records without logical images is
# still open.  The snapshot builder waits for it; its changes must reach the
# copy and never the decoding, both when it commits and when it aborts.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Replace the table name T in $sql.
sub sub_t
{
	my ($sql, $t) = @_;
	(my $x = $sql) =~ s/\bT(?=\b|_)/$t/g;
	return $x;
}

my $setup = q(
CREATE TABLE T (id int PRIMARY KEY, k int, v text) USING AM;
ALTER TABLE T ALTER COLUMN v SET STORAGE EXTERNAL;
CREATE INDEX T_k_idx ON T (k);
INSERT INTO T SELECT g, g, CASE WHEN g % 10 = 0 THEN repeat(md5(g::text), 100)
  ELSE 'v' || g END FROM generate_series(1, 200) g;
DELETE FROM T WHERE id % 7 = 0;
UPDATE T SET v = v || '+' WHERE id % 5 = 0;
UPDATE T SET k = k + 1000 WHERE id % 11 = 0;
);

# Committed while REPACK runs: INSERT; UPDATE in place, of an indexed
# column, of the key, growing, of a TOAST value; DELETE; and an INSERT that
# is updated and deleted again.
my $dml = q(
INSERT INTO T SELECT g, g, 'n' || g FROM generate_series(301, 320) g;
UPDATE T SET v = 'u' || id WHERE id BETWEEN 1 AND 6;
UPDATE T SET k = k + 2000 WHERE id BETWEEN 8 AND 12;
UPDATE T SET id = id + 1000 WHERE id BETWEEN 15 AND 18;
UPDATE T SET v = repeat('g', 300) WHERE id = 19;
UPDATE T SET v = repeat(md5('t'), 100) WHERE id = 20;
DELETE FROM T WHERE id BETWEEN 22 AND 26;
INSERT INTO T VALUES (400, 400, 'x');
UPDATE T SET v = 'y' WHERE id = 400;
DELETE FROM T WHERE id = 400;
);

# More DML once the table has been repacked.
my $after = q(
UPDATE T SET v = v || 'a';
DELETE FROM T WHERE id % 3 = 0;
INSERT INTO T SELECT g, g, 'a' FROM generate_series(501, 510) g;
UPDATE T SET k = k + 1 WHERE id IN (1, 2, 501);
);

sub create_twins
{
	my ($node, $prefix) = @_;
	foreach my $am ('flux', 'heap')
	{
		my $t = $prefix . '_' . substr($am, 0, 1);
		(my $s = $setup) =~ s/\bAM\b/$am/;
		$node->safe_psql('postgres', sub_t($s, $t));
	}
}

# Rows of a table as count, sum and md5, through a seqscan and through the
# primary key.
sub content_sql
{
	my ($t) = @_;
	my $q =
	  "SELECT count(*) || ':' || coalesce(sum(k), 0) || ':' || md5(coalesce(string_agg(x::text, ',' ORDER BY id), '')) FROM $t x";
	return
		"SET enable_indexscan = off; SET enable_bitmapscan = off; $q;\n"
	  . "SET enable_seqscan = off; SET enable_indexscan = on; $q;\n"
	  . "RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;";
}

sub content
{
	my ($node, $t) = @_;
	return $node->safe_psql('postgres', content_sql($t));
}

# bt_index_check (heapallindexed) reports corruption with an ERROR: returns
# psql's stderr, empty on success.
sub bt_check
{
	my ($node, $rel) = @_;
	my ($out, $err);
	my $rc = $node->psql(
		'postgres',
		"SELECT bt_index_check('$rel'::regclass, heapallindexed => true)",
		stdout => \$out,
		stderr => \$err,
		on_error_stop => 1);
	return $rc == 0 && $err eq '' ? '' : "rc=$rc: $err";
}

# Indexes intact, every key found by a unique check (primary only), and the
# heap twin passes verify_heapam.
sub check_table
{
	my ($node, $t, $what) = @_;
	my @idx = split /\n/,
	  $node->safe_psql('postgres',
		"SELECT c.relname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = '$t'::regclass ORDER BY 1"
	  );
	is(join(',', @idx), "${t}_k_idx,${t}_pkey", "$t: both indexes, $what");
	is(bt_check($node, $_), '', "$_: bt_index_check, $what") foreach @idx;
	is( $node->safe_psql(
			'postgres',
			"WITH i AS (INSERT INTO $t SELECT * FROM $t ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM i"
		),
		'0',
		"$t: unique checks see every row, $what")
	  unless $node->safe_psql('postgres', 'SELECT pg_is_in_recovery()') eq
	  't';
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM verify_heapam('$t')"),
		'0',
		"$t: verify_heapam, $what") if $t =~ /_h$/;
}

sub filenode
{
	my ($node, $t) = @_;
	return $node->safe_psql('postgres', "SELECT pg_relation_filenode('$t')");
}

# Files of the database that no relation owns, after a checkpoint has
# removed the files of dropped relations.
sub orphans
{
	my ($node) = @_;
	$node->safe_psql('postgres', 'CHECKPOINT');
	return $node->safe_psql(
		'postgres', q(
SELECT count(*) FROM pg_ls_dir('base/' || (SELECT oid FROM pg_database
                                           WHERE datname = current_database())) f
WHERE f ~ '^[0-9]+$' AND pg_filenode_relation(0, f::oid) IS NULL));
}

sub wait_pid
{
	my ($node, $pid, $cond, $what) = @_;
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) = 1 FROM pg_stat_activity WHERE pid = $pid AND $cond"
		),
		$what);
}

# Start REPACK (CONCURRENTLY) of $t in a background session that stops at
# the injection points in @points (attached for that session only).
sub start_repack
{
	my ($node, $t, @points) = @_;
	my $rp = $node->background_psql('postgres');
	my $pid = $rp->query_safe('SELECT pg_backend_pid()');
	$rp->query_safe('SELECT injection_points_set_local()') if @points;
	$rp->query_safe("SELECT injection_points_attach('$_', 'wait')")
	  foreach @points;
	$rp->query_until(qr/started/,
		"\\echo started\nREPACK (CONCURRENTLY) $t;\n\\echo repacked\n");
	return ($rp, $pid);
}

# Wait for the background REPACK to return; true if it succeeded.
sub finish_repack
{
	my ($rp) = @_;
	$rp->query_until(qr/repacked/, '');
	my $err = $rp->{stderr};
	$rp->quit;
	return $err;
}

# Is any undo, or any ATM entry, still holding WAL back?
my $retained = q(
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT bool_and(discard = insert) FROM pg_stat_get_undo_logs()
         WHERE persistence = 'permanent'));

###########################################################################
# Crash during REPACK (CONCURRENTLY)
###########################################################################
my $node = PostgreSQL::Test::Cluster->new('crash');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'injection_points or amcheck not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION amcheck;');

like(
	bt_check($node, 'pg_class'),
	qr/ERROR:  "pg_class" is not an index/,
	'control: bt_index_check of a table fails');

my @stages = qw(slot copied caught_up commit committed);
create_twins($node, "c_$_") foreach @stages;

sub crash_during_repack
{
	my ($stage, $t) = @_;
	my $oid = $node->safe_psql('postgres', "SELECT '$t'::regclass::oid");
	my $fn0 = filenode($node, $t);
	my $orph0 = orphans($node);
	my ($rp, $pid, $lk);

	if ($stage eq 'slot')
	{
		# The decoding worker creates the slot: attach for every process.
		$node->safe_psql('postgres',
			"SELECT injection_points_attach('replication-slot-create-begin', 'wait')"
		);
		($rp, $pid) = start_repack($node, $t);
		$node->wait_for_event('REPACK decoding worker',
			'replication-slot-create-begin');
	}
	else
	{
		($rp, $pid) = start_repack(
			$node, $t,
			'repack-concurrently-before-lock',
			$stage eq 'commit' ? ('commit-before-clog-update') : ());
		wait_pid(
			$node, $pid,
			"wait_event = 'repack-concurrently-before-lock'",
			"$t: REPACK copied the table");
	}
	# Control for the slot check below.
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_replication_slots WHERE slot_name LIKE 'pg_repack_%'"
		),
		'1',
		"$t: control: REPACK's slot exists before the crash"
	) unless $stage eq 'slot';
	$node->safe_psql('postgres', sub_t($dml, $t));
	my $expect = content($node, $t);

	if ($stage eq 'caught_up')
	{
		$lk = $node->background_psql('postgres');
		$lk->query_safe("BEGIN; LOCK TABLE $t IN ACCESS SHARE MODE");
	}
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('repack-concurrently-before-lock')")
	  unless $stage eq 'slot';
	if ($stage eq 'caught_up')
	{
		wait_pid(
			$node, $pid,
			"wait_event_type = 'Lock' AND wait_event = 'relation'",
			"$t: REPACK replayed the changes and waits for its lock");
		is( $node->safe_psql(
				'postgres',
				"SELECT phase, heap_tuples_inserted > 0, heap_tuples_updated > 0, heap_tuples_deleted > 0 FROM pg_stat_progress_repack WHERE pid = $pid"
			),
			'catch-up|t|t|t',
			"$t: control: REPACK replayed inserts, updates and deletes");
	}
	elsif ($stage eq 'commit')
	{
		wait_pid(
			$node, $pid,
			"wait_event = 'commit-before-clog-update'",
			"$t: REPACK flushed its commit record");
	}
	elsif ($stage eq 'committed')
	{
		is(finish_repack($rp), '', "$t: REPACK succeeded");
	}

	$node->stop('immediate');
	$rp->{run}->kill_kill if $stage ne 'committed';
	$lk->{run}->kill_kill if $lk;
	$node->start;

	my $what = "after a crash at $stage";
	if ($stage eq 'commit' || $stage eq 'committed')
	{
		isnt(filenode($node, $t), $fn0, "$t: REPACK committed, $what");
	}
	else
	{
		is(filenode($node, $t), $fn0, "$t: REPACK rolled back, $what");
	}
	is(content($node, $t), $expect, "$t: the committed rows, $what");
	check_table($node, $t, $what);
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_class WHERE relname = 'pg_temp_$oid'"),
		'0',
		"$t: no transient relation left, $what");
	is( $node->safe_psql(
			'postgres', 'SELECT count(*) FROM pg_replication_slots'),
		'0',
		"$t: no slot left, $what");
	ok( $node->poll_query_until('postgres', $retained),
		"$t: no undo holding WAL back, $what");
	return orphans($node) - $orph0;
}

# A crash leaves the files of a relation that an uncommitted transaction
# created behind (for heap as well; the same holds for a plain CREATE TABLE),
# so only the committed stages must leave nothing.
foreach my $stage (@stages)
{
	my %left =
	  map { $_ => crash_during_repack($stage, "c_${stage}_$_") } ('f', 'h');
	if ($stage eq 'commit' || $stage eq 'committed')
	{
		is($left{$_}, 0, "c_${stage}_$_: no file left behind after a crash")
		  foreach ('f', 'h');
	}
	else
	{
		is($left{f}, $left{h},
			"c_${stage}_f: leaves as many files behind as its heap twin");
		# Control: the transient table's files are counted.
		cmp_ok($left{h}, '>', 0,
			"c_${stage}_h: control: the transient table's files are left")
		  unless $stage eq 'slot';
	}
	is( content($node, "c_${stage}_f"),
		content($node, "c_${stage}_h"),
		"c_${stage}_f: same rows as its heap twin after a crash at $stage");
}

# Both tables can be repacked again and changed afterwards.
foreach my $stage (@stages)
{
	foreach my $am ('f', 'h')
	{
		my $t = "c_${stage}_$am";
		$node->safe_psql('postgres', "REPACK (CONCURRENTLY) $t");
		$node->safe_psql('postgres', sub_t($after, $t));
		check_table($node, $t, 'repacked again after the crash');
	}
	is( content($node, "c_${stage}_f"),
		content($node, "c_${stage}_h"),
		"c_${stage}_f: same rows as its heap twin, repacked again");
}

###########################################################################
# wal_level = replica: an open transaction wrote the table without logical
# images when REPACK enables logical decoding.
###########################################################################
# Before logical decoding is enabled: a same-size UPDATE (the byte-diff
# path), an UPDATE of an indexed column, an INSERT and a DELETE.
my $xbefore = q(
UPDATE T SET v = 'w' || substr(v, 2) WHERE id BETWEEN 1 AND 4;
UPDATE T SET k = k + 5000 WHERE id = 5;
INSERT INTO T VALUES (601, 601, 'x601');
DELETE FROM T WHERE id = 6;
);
# The same transaction, after REPACK enabled it.
my $xafter = q(
UPDATE T SET v = 'z' || substr(v, 2) WHERE id BETWEEN 1 AND 2;
INSERT INTO T VALUES (602, 602, 'x602');
DELETE FROM T WHERE id = 8;
);
foreach my $end ('commit', 'rollback')
{
	create_twins($node, "w_$end");
	foreach my $am ('f', 'h')
	{
		my $t = "w_${end}_$am";
		ok( $node->poll_query_until(
				'postgres',
				"SELECT current_setting('effective_wal_level') = 'replica'"),
			"$t: logical decoding is off");
		my $fn0 = filenode($node, $t);
		my $x = $node->background_psql('postgres');
		$x->query_safe("BEGIN;\n" . sub_t($xbefore, $t));
		my ($rp, $pid) = start_repack($node, $t);
		$node->wait_for_event('REPACK decoding worker', 'transactionid');
		is( $node->safe_psql(
				'postgres', "SELECT current_setting('effective_wal_level')"),
			'logical',
			"$t: REPACK enabled logical decoding, waits for the open transaction"
		);
		$x->query_safe(sub_t($xafter, $t) . "\n$end");
		$x->quit;
		is(finish_repack($rp), '', "$t: REPACK succeeded");
		isnt(filenode($node, $t), $fn0, "$t: repacked");
		check_table($node, $t,
			"transaction open across REPACK's start, $end");
	}
	is( content($node, "w_${end}_f"),
		content($node, "w_${end}_h"),
		"w_${end}_f: same rows as its heap twin");
	ok($node->poll_query_until('postgres', $retained),
		"w_$end: no undo holding WAL back");
}

###########################################################################
# Standby
###########################################################################
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION amcheck;');
$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'max_standby_streaming_delay = -1');
$standby->start;

create_twins($primary, 's');
$primary->wait_for_replay_catchup($standby);
foreach my $am ('f', 'h')
{
	my $t = "s_$am";
	my $sb = $standby->background_psql('postgres');
	$sb->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $old = $sb->query_safe(content_sql($t));
	my $fn0 = filenode($primary, $t);
	is($old, content($primary, $t), "$t: standby transaction reads the rows");

	my ($rp, $pid) =
	  start_repack($primary, $t, 'repack-concurrently-before-lock');
	wait_pid(
		$primary, $pid,
		"wait_event = 'repack-concurrently-before-lock'",
		"$t: REPACK copied the table");
	$primary->safe_psql('postgres', sub_t($dml, $t));
	$primary->safe_psql('postgres',
		"SELECT injection_points_wakeup('repack-concurrently-before-lock')");
	is(finish_repack($rp), '', "$t: REPACK succeeded");
	isnt(filenode($primary, $t), $fn0, "$t: repacked");

	ok( $standby->poll_query_until(
			'postgres',
			"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event_type = 'Lock' AND wait_event = 'relation'"
		),
		"$t: replay of the swap waits for the standby transaction");
	is($sb->query_safe(content_sql($t)),
		$old, "$t: the standby transaction still reads the old rows");
	$sb->query_safe('COMMIT');
	$sb->quit;

	$primary->wait_for_replay_catchup($standby);
	is( filenode($standby, $t),
		filenode($primary, $t),
		"$t: standby has the new file");
	is( content($standby, $t),
		content($primary, $t),
		"$t: standby has the primary's rows");
	check_table($standby, $t, 'on the standby');
	check_table($primary, $t, 'on the primary');
}
is( content($primary, 's_f'),
	content($primary, 's_h'),
	's_f: same rows as its heap twin');
is( content($standby, 's_f'),
	content($standby, 's_h'),
	's_f: same rows as its heap twin on the standby');

foreach my $n ($node, $primary, $standby)
{
	unlike(
		slurp_file($n->logfile),
		qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
		$n->name . ': no failed assertion, mismatched or abandoned undo');
}
$standby->stop;
$primary->stop;
$node->stop;
done_testing();
