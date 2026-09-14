
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# The row locks and row changes of a PREPARED transaction survive an
# immediate restart and a standby promotion, as heap's do.  Heap is the
# control for every case.
#
# FLUX keeps a row lock's owner and strength only in the shared-memory tuple
# sLog; the page carries an ownerless FLUX_TUPLE_LOCKED bit.  A restart
# empties the sLog, so a prepared transaction's FLUX row locks used to
# vanish: an UPDATE of a FLUX row the prepared transaction had locked FOR
# UPDATE succeeded after the restart, while the same UPDATE of a heap row
# still blocked.  Each PREPARE also leaked one count of FLUX's active
# transactions.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_prepared_transactions = 40
));
$node->start;
if (   !$node->check_extension('amcheck')
	|| !$node->check_extension('test_idxundo'))
{
	plan skip_all => 'amcheck or test_idxundo not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION amcheck; CREATE EXTENSION test_idxundo;');

my @ams = ('heap', 'flux');
foreach my $am (@ams)
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE t_$am (id int PRIMARY KEY, v int) USING $am;
INSERT INTO t_$am SELECT g, g FROM generate_series(1, 80) g;});
}

$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

# Run sql in a transaction that is rolled back, giving up after a short
# lock_timeout.  Reports whether it had to wait for a lock.
sub probe
{
	my ($n, $sql) = @_;
	my ($ret, $stdout, $stderr) = $n->psql('postgres',
		"BEGIN; SET LOCAL lock_timeout = '300ms'; $sql; ROLLBACK;");
	return 'blocks' if $stderr =~ /canceling statement due to lock timeout/;
	return 'proceeds' if $stderr eq '';
	return "error: $stderr";
}

sub prepare
{
	my ($gid, @sql) = @_;
	$node->safe_psql('postgres',
		join(";\n", 'BEGIN', @sql, "PREPARE TRANSACTION '$gid'"));
}

my @modes = ('FOR UPDATE', 'FOR NO KEY UPDATE', 'FOR SHARE', 'FOR KEY SHARE');
my @probes = (
	[ 'non-key UPDATE', 'UPDATE %s SET v = v + 1 WHERE id = %d' ],
	[ 'DELETE', 'DELETE FROM %s WHERE id = %d' ],
	[ 'FOR UPDATE', 'SELECT 1 FROM %s WHERE id = %d FOR UPDATE' ],
	[ 'FOR SHARE', 'SELECT 1 FROM %s WHERE id = %d FOR SHARE' ],
	[ 'FOR KEY SHARE', 'SELECT 1 FROM %s WHERE id = %d FOR KEY SHARE' ],);
# A key-changing UPDATE is left out: FLUX does not make it wait for a FOR KEY
# SHARE lock even without two-phase commit, a separate bug.
# Which probes a lock in each mode blocks: heap's tuple lock conflict table.
my %blocks = (
	'FOR UPDATE' => [ 1, 1, 1, 1, 1 ],
	'FOR NO KEY UPDATE' => [ 1, 1, 1, 1, 0 ],
	'FOR SHARE' => [ 1, 1, 1, 0, 0 ],
	'FOR KEY SHARE' => [ 0, 1, 1, 0, 0 ],);

# Run every probe against row $id of t_$am and compare with what a lock in
# $mode should block ($mode undef: nothing is locked).
sub check_probes
{
	my ($n, $am, $id, $mode, $what) = @_;
	for my $p (0 .. $#probes)
	{
		my ($pname, $fmt) = @{ $probes[$p] };
		my $want =
		  (defined $mode && $blocks{$mode}[$p]) ? 'blocks' : 'proceeds';
		is(probe($n, sprintf($fmt, "t_$am", $id)),
			$want, "$what: $pname of the $am row");
	}
}

# Visible rows of the rows the DML transactions touch, by seq scan and by
# index scan.
my $dmlrows = '41, 42, 51, 52, 90, 91';
# $n is a node or, to read in an open transaction, a background session.
sub dml_state
{
	my ($n, $am) = @_;
	my $q =
	  "SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t_$am WHERE id IN ($dmlrows)";
	my @sql = (
		"SET enable_indexscan = off; SET enable_bitmapscan = off; $q",
		"SET enable_seqscan = off; SET enable_indexscan = on; $q");
	my @out = map {
			$n->isa('PostgreSQL::Test::Cluster')
		  ? $n->safe_psql('postgres', $_)
		  : $n->query_safe($_)
	} @sql;
	$n->query_safe(
		'RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan'
	) unless $n->isa('PostgreSQL::Test::Cluster');
	return "seq $out[0] idx $out[1]";
}

# Row $id of t_$am is locked in mode $i by gid lock_$am_$i, resolved after
# the restart; rows 11..14 by gid early_$am_$i, resolved before it.
foreach my $am (@ams)
{
	for my $i (0 .. $#modes)
	{
		prepare("lock_${am}_$i",
			"SELECT 1 FROM t_$am WHERE id = " . (1 + $i) . " $modes[$i]");
		prepare("early_${am}_$i",
			"SELECT 1 FROM t_$am WHERE id = " . (11 + $i) . " $modes[$i]");
	}

	# Two prepared transactions share a FOR SHARE lock on row 20.
	prepare("share_a_$am", "SELECT 1 FROM t_$am WHERE id = 20 FOR SHARE");
	prepare("share_b_$am", "SELECT 1 FROM t_$am WHERE id = 20 FOR SHARE");

	# Prepared row changes: one transaction is committed after the restart,
	# the other rolled back.
	prepare(
		"dml_c_$am",
		"INSERT INTO t_$am VALUES (90, 90)",
		"UPDATE t_$am SET v = v + 100 WHERE id = 41",
		"DELETE FROM t_$am WHERE id = 42");
	prepare(
		"dml_r_$am",
		"INSERT INTO t_$am VALUES (91, 91)",
		"UPDATE t_$am SET v = v + 100 WHERE id = 51",
		"DELETE FROM t_$am WHERE id = 52");
}

foreach my $am (@ams)
{
	prepare(
		"upgrade_$am",
		"SELECT 1 FROM t_$am WHERE id = 75 FOR KEY SHARE",
		'SAVEPOINT a',
		"SELECT 1 FROM t_$am WHERE id = 75 FOR UPDATE",
		'ROLLBACK TO a');
	check_probes(
		$node, $am, 75,
		'FOR KEY SHARE',
		'rolled-back upgrade before restart');
}

# One transaction locks a heap row and a FLUX row.
prepare(
	'mixed',
	'SELECT 1 FROM t_heap WHERE id = 30 FOR UPDATE',
	'SELECT 1 FROM t_flux WHERE id = 30 FOR UPDATE');

# Resolved on the promoted standby.
prepare(
	'sb',
	'SELECT 1 FROM t_heap WHERE id = 60 FOR UPDATE',
	'SELECT 1 FROM t_flux WHERE id = 60 FOR UPDATE',
	'SELECT 1 FROM t_heap WHERE id = 61 FOR KEY SHARE',
	'SELECT 1 FROM t_flux WHERE id = 61 FOR KEY SHARE');

# Before the restart.
foreach my $am (@ams)
{
	for my $i (0 .. $#modes)
	{
		check_probes($node, $am, 1 + $i, $modes[$i],
			"before restart, $modes[$i]");

		my $early = 11 + $i;
		is(probe($node, "DELETE FROM t_$am WHERE id = $early"),
			'blocks', "before restart, early $modes[$i]: $am row locked");
		$node->safe_psql('postgres',
			($i % 2 ? 'COMMIT' : 'ROLLBACK') . " PREPARED 'early_${am}_$i'");
		check_probes($node, $am, $early, undef,
			"before restart, early $modes[$i] resolved");
	}

	is(probe($node, "SELECT 1 FROM t_$am WHERE id = 20 FOR SHARE"),
		'proceeds', "before restart, shared FOR SHARE: $am FOR SHARE");
	is(probe($node, "UPDATE t_$am SET v = v WHERE id = 20"),
		'blocks', "before restart, shared FOR SHARE: $am UPDATE");

	is( dml_state($node, $am),
		'seq 41:41,42:42,51:51,52:52 idx 41:41,42:42,51:51,52:52',
		"before restart, prepared row changes are invisible in $am");
}

# Immediate restart.  The checkpoint leaves nothing to redo, so whatever
# FLUX needs must come from the pages, CLOG and the two-phase state files.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;

foreach my $am (@ams)
{
	for my $i (0 .. $#modes)
	{
		check_probes($node, $am, 1 + $i, $modes[$i],
			"after restart, $modes[$i]");
	}

	# Shared FOR SHARE: still blocked after one of the two holders goes.
	is(probe($node, "SELECT 1 FROM t_$am WHERE id = 20 FOR SHARE"),
		'proceeds', "after restart, shared FOR SHARE: $am FOR SHARE");
	is(probe($node, "UPDATE t_$am SET v = v WHERE id = 20"),
		'blocks', "after restart, shared FOR SHARE: $am UPDATE");
	$node->safe_psql('postgres', "COMMIT PREPARED 'share_a_$am'");
	is(probe($node, "UPDATE t_$am SET v = v WHERE id = 20"),
		'blocks',
		"after restart, shared FOR SHARE, one committed: $am UPDATE");
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'share_b_$am'");
	is(probe($node, "UPDATE t_$am SET v = v WHERE id = 20"),
		'proceeds',
		"after restart, shared FOR SHARE, both resolved: $am UPDATE");

	# Prepared row changes: invisible, and writers wait for them.
	is( dml_state($node, $am),
		'seq 41:41,42:42,51:51,52:52 idx 41:41,42:42,51:51,52:52',
		"after restart, prepared row changes are invisible in $am");
	foreach my $id (41, 51)
	{
		is(probe($node, "UPDATE t_$am SET v = v WHERE id = $id"),
			'blocks', "after restart, UPDATE of a prepared UPDATE's $am row");
		is(probe($node, "DELETE FROM t_$am WHERE id = $id"),
			'blocks', "after restart, DELETE of a prepared UPDATE's $am row");
	}
	foreach my $id (42, 52)
	{
		is(probe($node, "UPDATE t_$am SET v = v WHERE id = $id"),
			'blocks', "after restart, UPDATE of a prepared DELETE's $am row");
	}
	foreach my $id (90, 91)
	{
		is(probe($node, "INSERT INTO t_$am VALUES ($id, 0)"),
			'blocks', "after restart, INSERT of a prepared INSERT's $am key");
	}
}

foreach my $am (@ams)
{
	check_probes(
		$node, $am, 75,
		'FOR KEY SHARE',
		'rolled-back upgrade after restart');
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'upgrade_$am'");
	check_probes($node, $am, 75, undef, 'rolled-back upgrade resolved');
}

# A session waiting on the mixed transaction's locks proceeds when it
# commits.
my %waiter;
foreach my $am (@ams)
{
	$waiter{$am} = $node->background_psql('postgres');
	$waiter{$am}->query_until(qr/start/,
		"\\echo start\nUPDATE t_$am SET v = v + 1000 WHERE id = 30;\n");
}
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE 'UPDATE t_% SET v = v + 1000%'"
	),
	'after restart, UPDATEs of the mixed rows wait');
$node->safe_psql('postgres', "COMMIT PREPARED 'mixed'");
foreach my $am (@ams)
{
	$waiter{$am}->query_safe('SELECT 1');
	$waiter{$am}->quit;
	is( $node->safe_psql('postgres', "SELECT v FROM t_$am WHERE id = 30"),
		'1030',
		"the waiter's UPDATE of the $am row ran after COMMIT PREPARED");
}

# REPEATABLE READ snapshots taken after the restart, before COMMIT PREPARED,
# must keep seeing the prepared row changes as uncommitted.
my %rr;
foreach my $am (@ams)
{
	$rr{$am} = $node->background_psql('postgres');
	$rr{$am}->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$rr{$am}->query_safe("SELECT count(*) FROM t_$am");
}

# Resolve the rest.
foreach my $am (@ams)
{
	for my $i (0 .. $#modes)
	{
		$node->safe_psql('postgres',
			($i % 2 ? 'COMMIT' : 'ROLLBACK') . " PREPARED 'lock_${am}_$i'");
		check_probes($node, $am, 1 + $i, undef,
			"after restart, $modes[$i] resolved");
	}

	$node->safe_psql('postgres', "COMMIT PREPARED 'dml_c_$am'");
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'dml_r_$am'");
	is( dml_state($node, $am),
		'seq 41:141,51:51,52:52,90:90 idx 41:141,51:51,52:52,90:90',
		"after restart, COMMIT and ROLLBACK PREPARED row changes in $am");
	# Read every row again, setting whatever hints the readers set.
	$node->safe_psql('postgres', "SELECT * FROM t_$am");
	$node->safe_psql('postgres',
		"SELECT * FROM t_$am WHERE id IN ($dmlrows)");
	is( dml_state($rr{$am}, $am),
		'seq 41:41,42:42,51:51,52:52 idx 41:41,42:42,51:51,52:52',
		"after restart, an older snapshot does not see the resolved $am changes"
	);
	$rr{$am}->query_safe('COMMIT');
	$rr{$am}->quit;
	foreach my $id (41, 51, 52, 90)
	{
		is(probe($node, "UPDATE t_$am SET v = v WHERE id = $id"),
			'proceeds',
			"after restart and resolution, UPDATE of $am row $id");
	}
	is(probe($node, "INSERT INTO t_$am VALUES (91, 0)"),
		'proceeds', "after restart and resolution, INSERT of $am key 91");
}
is( $node->safe_psql(
		'postgres',
		'SELECT count(*) FROM (SELECT * FROM t_heap EXCEPT SELECT * FROM t_flux) x'
	),
	'0',
	'heap and FLUX tables match');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM t_heap'),
	$node->safe_psql('postgres', 'SELECT count(*) FROM t_flux'),
	'heap and FLUX row counts match');
$node->safe_psql('postgres', "SELECT bt_index_check('t_flux_pkey', true)");

# FLUX's count of active transactions does not grow with each PREPARE.
my $active0 =
  $node->safe_psql('postgres', 'SELECT idxundo_flux_active_xacts()');
for my $i (1 .. 3)
{
	prepare("leak_$i", 'UPDATE t_flux SET v = v WHERE id = 70');
	$node->safe_psql('postgres', "COMMIT PREPARED 'leak_$i'");
}
is($node->safe_psql('postgres', 'SELECT idxundo_flux_active_xacts()'),
	$active0, 'PREPARE does not leak a FLUX active transaction');

# The standby promoted with 'sb' still prepared holds its locks.
$node->wait_for_replay_catchup($standby);
$standby->promote;
foreach my $am (@ams)
{
	check_probes($standby, $am, 60, 'FOR UPDATE',
		'promoted standby, FOR UPDATE');
	check_probes(
		$standby, $am, 61,
		'FOR KEY SHARE',
		'promoted standby, FOR KEY SHARE');
}
$standby->safe_psql('postgres', "COMMIT PREPARED 'sb'");
foreach my $am (@ams)
{
	check_probes($standby, $am, 60, undef, 'promoted standby, resolved');
	check_probes($standby, $am, 61, undef, 'promoted standby, resolved');
}

done_testing();
