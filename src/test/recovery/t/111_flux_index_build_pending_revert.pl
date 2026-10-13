# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Index builds, ANALYZE and TABLESAMPLE on a FLUX table while an aborted
# transaction's revert is still pending.
#
# FLUX updates in place, so a transaction whose rollback was handed to an
# undo apply worker (undo_rollback_overflow_size = 0) leaves its images on
# the page until the worker applies the undo.  A reader that looks at the
# page itself instead of through an MVCC snapshot must still see the row as
# the rollback will leave it: an aborted UPDATE's row at its before-image, an
# aborted DELETE's row alive, an aborted INSERT's row gone.  An index built
# from the aborted images stays wrong after the revert, because the revert
# reverses index changes the new index never had.
#
# Each FLUX table has a heap twin that runs the same statements; the heap
# twin is the control.  The undo apply worker is held at the injection point
# pbu-undo-worker-before-apply (see 106_flux_rewrite_pending_revert.pl), the
# checks run while it is held, after it has applied the undo, and after more
# DML.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Index kinds.  idx creates the index T_i on table T; pred(x) is a qual the
# index answers for the key x; order, if set, is an ORDER BY the index
# answers (with where, its qual); bt says amcheck can check it.
my %kind = (
	btree => {
		idx => 'CREATE INDEX T_i ON T (k)',
		pred => sub { "k = $_[0]" },
		order => 'k',
		bt => 1
	},
	hash => {
		idx => 'CREATE INDEX T_i ON T USING hash (k)',
		pred => sub { "k = $_[0]" }
	},
	gist => {
		idx => "CREATE INDEX T_i ON T USING gist (int4range(k, k, '[]'))",
		pred =>
		  sub { "int4range(k, k, '[]') && int4range($_[0], $_[0], '[]')" }
	},
	unique => {
		idx => 'CREATE UNIQUE INDEX T_i ON T (k)',
		pred => sub { "k = $_[0]" },
		order => 'k',
		bt => 1
	},
	expr => {
		idx => 'CREATE INDEX T_i ON T ((k * 2))',
		pred => sub { "k * 2 = 2 * $_[0]" },
		order => 'k * 2',
		bt => 1
	},
	partial => {
		idx => 'CREATE INDEX T_i ON T (k) WHERE k < 1000',
		pred => sub { "k = $_[0] AND k < 1000" },
		order => 'k',
		where => 'k < 1000',
		bt => 1
	},
	multi => {
		idx => 'CREATE INDEX T_i ON T (k, v)',
		pred => sub { "k = $_[0]" },
		order => 'k, v',
		bt => 1
	},);

# name => [index kind, index exists before the abort, statement after it]
my @cases = (
	(map { [ "create_$_", $_, 0, $kind{$_}{idx} ] } sort keys %kind),
	[ reindex_index => 'btree', 1, 'REINDEX INDEX T_i' ],
	[ reindex_table => 'btree', 1, 'REINDEX TABLE T' ],
	[ reindex_conc => 'btree', 1, 'REINDEX INDEX CONCURRENTLY T_i' ],
	# Its validate scan (table_index_validate_scan) runs as well.
	[ cic => 'btree', 0, 'CREATE INDEX CONCURRENTLY T_i ON T (k)' ],
	[
		parallel => 'btree',
		0,
		"SET client_min_messages = debug1;\nCREATE INDEX T_i ON T (k)"
	],
	[
		parallel_unique => 'unique',
		0,
		"SET client_min_messages = debug1;\nCREATE UNIQUE INDEX T_i ON T (k)"
	],);

# Run $sql once per table, T replaced by the table name.
sub each_tab
{
	my ($sql, @tabs) = @_;
	return join("\n", map { (my $x = $sql) =~ s/\bT(?=\b|_)/$_/g; $x } @tabs);
}

# 2000 rows, about 60 pages: enough for a parallel build.  v = id % 50, so
# every value of v is equally common.
my $setup = q(
CREATE TABLE T (id int PRIMARY KEY, k int, v int, pad text) USING AM;
INSERT INTO T SELECT g, g, g % 50, repeat('p', 200) FROM generate_series(1, 2000) g;
);

# The aborted transaction.  424242 and 31337 are values no committed row
# has; v = 13 is a value it deletes.
my $aborted = q(
UPDATE T SET k = k + 1000000, v = 424242 WHERE id IN (3, 4, 1500);
UPDATE T SET k = 20 WHERE id = 8;
UPDATE T SET v = 424242 WHERE id BETWEEN 100 AND 160;
DELETE FROM T WHERE id IN (5, 1600);
DELETE FROM T WHERE v = 13;
INSERT INTO T SELECT g, g, 31337, 'ins' FROM generate_series(5001, 5060) g;
UPDATE T SET k = k + 1 WHERE id = 5001;
UPDATE T SET v = 71 WHERE id = 7;
UPDATE T SET v = 72 WHERE id = 7;
UPDATE T SET k = k + 2000000 WHERE id = 9;
DELETE FROM T WHERE id = 9;
);

my $after = q(
UPDATE T SET k = k + 10000 WHERE id IN (3, 4, 8, 120, 1500);
DELETE FROM T WHERE id IN (7, 1600);
INSERT INTO T VALUES (6001, 6001, 1, 'a');
);

# Committed and aborted keys.
my @probes = (
	3, 4, 5, 7, 8, 9, 13, 20, 63, 100, 113, 120, 160, 999, 1500, 1600, 2000,
	1000003, 1000004, 1001500, 2000009, 5001, 5002, 5060, 10003, 6001);

# enable_sort = off only for ORDER BY: with it, the probes' string_agg
# (... ORDER BY) would rather read presorted rows through the primary key.
my $idx_probe = "SET enable_seqscan = off; SET enable_bitmapscan = off;\n";
my $idx_on = $idx_probe . "SET enable_sort = off;\n";
my $seq_on = "SET enable_indexscan = off; SET enable_bitmapscan = off;\n";

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 2
min_parallel_table_scan_size = 0
maintenance_work_mem = '256MB'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'injection_points or amcheck not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION amcheck;
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
));

# stdout of $sql, and stderr if there is any.  Never dies.
sub run
{
	my ($sql) = @_;
	my ($out, $err) = ('', '');
	my $rc = $node->psql(
		'postgres', $sql,
		stdout => \$out,
		stderr => \$err,
		on_error_stop => 1);
	return $err eq '' && $rc == 0 ? $out : "$out\nrc=$rc stderr: $err";
}

# bt_index_check (heapallindexed) of one index: '' on success.
sub bt_check
{
	my ($rel) = @_;
	my ($out, $err) = ('', '');
	my $rc = $node->psql(
		'postgres',
		"SELECT bt_index_check('$rel'::regclass, heapallindexed => true)",
		stdout => \$out,
		stderr => \$err,
		on_error_stop => 1);
	return $rc == 0 && $err eq '' ? '' : "rc=$rc: $err";
}

sub content
{
	my ($t) = @_;
	return run($seq_on
		  . "SELECT count(*) || ':' || md5(coalesce(string_agg(id || ':' || k || ':' || v || ':' || pad, ',' ORDER BY id), '')) FROM $t"
	);
}

# One probe: the rows the qual finds for key $x.
sub probe
{
	my ($t, $kind, $x) = @_;
	return
	  "SELECT '$x=' || coalesce(string_agg(r, ',' ORDER BY r), '') FROM (SELECT id || ':' || k || ':' || v AS r FROM $t WHERE "
	  . $kind{$kind}{pred}->($x) . ') s;';
}

# Every probe.
sub probes
{
	my ($t, $kind) = @_;
	return join("\n", map { probe($t, $kind, $_) } @probes);
}

sub ordered
{
	my ($t, $kind) = @_;
	my $k = $kind{$kind};
	my $w = $k->{where} ? "WHERE $k->{where} " : '';
	return "SELECT id, k, v FROM $t ${w}ORDER BY $k->{order}";
}

sub check_case
{
	my ($c, $what) = @_;
	my ($name, $kind) = @$c;
	my ($f, $h) = ("f_$name", "h_$name");

	is(content($f), content($h), "$f: same rows as $h, $what");

	foreach my $t ($f, $h)
	{
		like(
			run($idx_probe . 'EXPLAIN (COSTS OFF) ' . probe($t, $kind, 3)),
			qr/Index Scan using ${t}_i /,
			"$t: the probes use the index, $what");
	}
	my $fi = run($idx_probe . probes($f, $kind));
	is( $fi,
		run($seq_on . probes($f, $kind)),
		"$f: index scan finds what a seqscan finds, $what");
	is( $fi,
		run($idx_probe . probes($h, $kind)),
		"$f: index scan finds what $h finds, $what");

	if ($kind{$kind}{order})
	{
		like(
			run($idx_on . 'EXPLAIN (COSTS OFF) ' . ordered($f, $kind)),
			qr/^\s*Index Scan using ${f}_i /m,
			"$f: ORDER BY uses the index, $what");
		my $fo = run($idx_on . ordered($f, $kind));
		is( $fo,
			run($seq_on . ordered($f, $kind)),
			"$f: ORDER BY through the index = sorted seqscan, $what");
		is( $fo,
			run($idx_on . ordered($h, $kind)),
			"$f: ORDER BY through the index = $h, $what");
	}

	if ($kind{$kind}{bt})
	{
		foreach my $t ($f, $h)
		{
			is(bt_check($_), '', "$_: bt_index_check, $what")
			  foreach ("${t}_i", "${t}_pkey");
		}
	}
}

# ANALYZE must not see the aborted values, and must see the rows the
# aborted DELETEs did not delete.
my $stats_q = q(
SELECT attname, null_frac, n_distinct, most_common_vals, most_common_freqs,
       histogram_bounds
FROM pg_stats WHERE schemaname = 'public' AND tablename = 'T' AND attname IN ('k', 'v')
ORDER BY attname;
SELECT reltuples FROM pg_class WHERE relname = 'T';
);

sub check_stats
{
	my ($what) = @_;
	$node->safe_psql('postgres', 'ANALYZE f_stats; ANALYZE h_stats');
	my $fs = run(each_tab($stats_q, 'f_stats'));
	is( $fs,
		run(each_tab($stats_q, 'h_stats')),
		"f_stats: ANALYZE statistics = h_stats, $what");
	unlike(
		$fs,
		qr/424242|31337|100000[34]|1001500/,
		"f_stats: no aborted value in the statistics, $what");

	foreach my $m ('BERNOULLI (100)', 'SYSTEM (100)')
	{
		my $q =
		  "SELECT count(*) || ':' || md5(string_agg(id || ':' || k || ':' || v, ',' ORDER BY id)) FROM T TABLESAMPLE $m";
		is( run(each_tab($q, 'f_stats')),
			run(each_tab($q, 'h_stats')),
			"f_stats: TABLESAMPLE $m = h_stats, $what");
	}
}

my $retained = q(
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT bool_and(discard = insert) FROM pg_stat_get_undo_logs()
         WHERE persistence = 'permanent'));

my $held =
  "SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'pbu-undo-worker-before-apply'";

###########################################################################
# Set up, abort, build while the revert is pending.
###########################################################################
my @tabs;
foreach my $c (@cases, [ 'stats', undef, 0 ])
{
	foreach my $am ('flux', 'heap')
	{
		my $t = substr($am, 0, 1) . "_$c->[0]";
		(my $s = $setup) =~ s/\bAM\b/$am/;
		$s .= $kind{ $c->[1] }{idx} . ";\n" if $c->[2];
		$node->safe_psql('postgres', each_tab($s, $t));
		push @tabs, $t;
	}
}

$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
$node->safe_psql('postgres',
	"BEGIN;\n" . each_tab($aborted, @tabs) . "\nROLLBACK;");
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');

foreach my $c (@cases)
{
	foreach my $t ("f_$c->[0]", "h_$c->[0]")
	{
		my ($out, $err) = ('', '');
		my $rc = $node->psql(
			'postgres', each_tab($c->[3], $t),
			stdout => \$out,
			stderr => \$err,
			on_error_stop => 1);
		is($rc, 0, "$t: $c->[0] ran") or diag($err);
		like(
			$err,
			qr/building index "${t}_i" on table "$t" with request for 2 parallel workers/,
			"$t: the build is parallel") if $c->[0] =~ /^parallel/;
	}
}

is($node->safe_psql('postgres', $held),
	'1', 'the undo apply worker is still held');
check_case($_, 'built while the revert was pending') foreach @cases;
check_stats('while the revert is pending');
is($node->safe_psql('postgres', $held),
	'1', 'the undo apply worker is still held after the checks');

# get_actual_variable_endpoint (selfuncs.c) reads the index with
# SnapshotNonVacuumable through the index-only-scan machinery, so it would
# see whatever the table AM reports for that snapshot.  It is not reachable
# for a FLUX table: get_relation_info sets canreturn = false for every
# index of a delete-marking table (a delete-marked entry can carry a stale
# key), and get_actual_variable_range skips indexes with !canreturn[0].
# Show that: no index-only scan for FLUX, while the heap twin has one.
{
	my $q = "${idx_on}EXPLAIN (COSTS OFF) SELECT k FROM T WHERE k > 1999000";
	unlike(
		run(each_tab($q, 'f_create_btree')),
		qr/Index Only Scan/,
		'FLUX: no index-only scan, so no endpoint probe');
	like(
		run(each_tab($q, 'h_create_btree')),
		qr/Index Only Scan/,
		'heap: index-only scan (control)');
}

# Control: bt_check must be able to fail.
like(
	bt_check('h_stats'),
	qr/"h_stats" is not an index/,
	'control: bt_index_check of a table fails');

###########################################################################
# Let the undo apply worker go.
###########################################################################
$node->safe_psql('postgres',
	"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
ok($node->poll_query_until('postgres', 'SELECT release_one()'),
	'held undo apply worker released');
ok($node->poll_query_until('postgres', $retained), 'the undo was applied');
check_case($_, 'after the undo apply worker') foreach @cases;
check_stats('after the undo apply worker');

$node->safe_psql('postgres', each_tab($after, @tabs));
check_case($_, 'after more DML') foreach @cases;

unlike(
	slurp_file($node->logfile),
	qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
	'no failed assertion, mismatched or abandoned undo');
$node->stop;
done_testing();
