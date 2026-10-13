
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Running the row-lock store out of shared memory fails the statement before
# anything changes, and the cluster carries on.
#
# A FLUX row lock records its holder in the tuple sLog under the tuple's
# buffer lock before it marks the page.  An injection point fails the
# allocation of the shared tree that a transaction's further locks go to:
# the failing lock must leave no FLUX_TUPLE_LOCKED bit and no WAL behind, the
# locks the transaction already held must be released by the abort, and a
# later transaction must lock the same rows normally.  The first few locks of
# a transaction need no allocation, so they succeed with the injection point
# attached; how many is not part of the contract, so the test finds the first
# failing row rather than assuming it.
#
# Then the store's allocations themselves fail, part way through a lock that
# needs two (a bigger relation array, then a table), and part way through a
# transaction whose table must grow, at a new key and at a stronger lock on
# a key it holds.  A savepoint catches each failure; the transaction's
# earlier locks must all still be found, the failed ones must not, and the
# transaction must go on locking, then end with nothing left.  Repeated
# failures must not use up shared memory.  (Heap has no such store: these
# parts have no heap control.)
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('pageinspect')
	|| !$node->check_extension('pg_walinspect'))
{
	plan skip_all =>
	  'injection_points, pageinspect or pg_walinspect not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION pageinspect; CREATE EXTENSION pg_walinspect;'
);
$node->safe_psql(
	'postgres', q{
CREATE TABLE t (id int PRIMARY KEY, v int) USING flux;
INSERT INTO t SELECT g, g FROM generate_series(1, 40) g;
CHECKPOINT;
});

# FLUX_TUPLE_LOCKED is 0x0010 of t_flags, whose low byte is at byte 32 of
# the tuple.
my $locked_expr = "get_byte(get_raw_page('t', 0), lp_off + 32) & 16 <> 0";

# Number of tuples on block 0 marked locked
sub locked_bits
{
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM heap_page_items(get_raw_page('t', 0)) WHERE lp_flags = 1 AND $locked_expr"
	);
}
my $bits_before = locked_bits();

# (a record written now is the start of the range read below)
$node->safe_psql('postgres', 'SELECT pg_switch_wal(); CHECKPOINT;');
my $lsn_before =
  $node->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

# Lock rows one by one in one transaction until a lock fails.
my $h = $node->background_psql('postgres', on_error_stop => 0);
$h->query_safe(
	"SELECT injection_points_attach('slog-tuple-lock-oom', 'notice')");
$h->query_safe('BEGIN');
my $ok = 0;
my $err = '';
foreach my $i (1 .. 40)
{
	my ($out, $failed) =
	  $h->query("SELECT id FROM t WHERE id = $i FOR UPDATE");
	if ($failed && $h->{stderr} =~ /out of memory/)
	{
		$err = $h->{stderr};
		last;
	}
	$h->{stderr} = '';
	$ok++;
}
like(
	$err,
	qr/out of memory/,
	'a row lock fails once the store is out of memory');
note "$ok row locks succeeded before the failure";
$h->{stderr} = '';
$h->query_safe('ROLLBACK');
$h->query_safe("SELECT injection_points_detach('slog-tuple-lock-oom')");

# The failed lock changed nothing on the page: the only LOCKED bits are those
# of the locks that succeeded (they stay until the page is next written).
my $failed_id = $ok + 1;
is( $node->safe_psql(
		'postgres',
		"SELECT $locked_expr FROM heap_page_items(get_raw_page('t', 0)) WHERE lp = (SELECT (ctid::text::point)[1]::int FROM t WHERE id = $failed_id)"
	),
	'f',
	'the row whose lock failed was not marked locked');
cmp_ok(
	locked_bits(), '<=',
	$bits_before + $ok,
	'no row beyond those locked was marked');

# No lock record was logged for the failed row.  (The transaction rolled
# back, and an abort does not flush WAL: flush it before reading it.)
$node->safe_psql('postgres', 'CHECKPOINT');
my $nlock = $node->safe_psql(
	'postgres', qq{
SELECT count(*) FROM pg_get_wal_records_info('$lsn_before', pg_current_wal_flush_lsn())
WHERE resource_manager = 'FLUX' AND record_type = 'LOCK'});
is($nlock, $ok,
	'one lock record per successful lock, none for the failed one');

# The aborted transaction holds nothing; a new one locks every row.
my ($ret, $out, $stderr) = $node->psql('postgres',
	"SET lock_timeout = '1s'; BEGIN; SELECT count(*) FROM (SELECT 1 FROM t FOR UPDATE) s; COMMIT;"
);
is($ret, 0, 'after the failure every row can be locked again');
like($out, qr/^40$/m, 'all 40 rows locked by a later transaction');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'40', 'table intact');

$h->quit;

# Count rows lo..hi of rel that another session cannot lock in mode.
sub blocked
{
	my ($rel, $lo, $hi, $mode) = @_;
	my ($stdout, $stderr);
	$node->psql(
		'postgres', qq{
SET client_min_messages = notice;
DO \$\$
DECLARE i int; n int := 0;
BEGIN
  FOR i IN $lo..$hi LOOP
    BEGIN
      PERFORM 1 FROM $rel WHERE id = i FOR $mode NOWAIT;
    EXCEPTION WHEN lock_not_available THEN n := n + 1;
    END;
  END LOOP;
  RAISE NOTICE 'blocked %', n;
END \$\$;
},
		stdout => \$stdout,
		stderr => \$stderr);
	$stderr =~ /blocked (\d+)/ or die "no count for $rel: $stderr";
	return $1;
}

sub dsa_size
{
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(size), 0) FROM pg_dsm_registry_allocations WHERE name = 'slog_tuple_lock_tables'"
	);
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE u (id int PRIMARY KEY, v int) USING flux;
INSERT INTO u SELECT g, g FROM generate_series(1, 300) g;
CREATE TABLE w1 (id int PRIMARY KEY) USING flux;
CREATE TABLE w2 (id int PRIMARY KEY) USING flux;
CREATE TABLE w3 (id int PRIMARY KEY) USING flux;
CREATE TABLE w4 (id int PRIMARY KEY) USING flux;
CREATE TABLE w5 (id int PRIMARY KEY) USING flux;
INSERT INTO w1 SELECT generate_series(1, 10);
INSERT INTO w2 SELECT generate_series(1, 10);
INSERT INTO w3 SELECT generate_series(1, 10);
INSERT INTO w4 SELECT generate_series(1, 10);
INSERT INTO w5 SELECT generate_series(1, 10);
});

# Run sql in session h with the injection point attached; return its error.
sub fails
{
	my ($h, $point, $sql) = @_;
	$h->query_safe("SELECT injection_points_attach('$point', 'notice')");
	$h->query_safe('SAVEPOINT f');
	$h->{stderr} = '';
	$h->query($sql);
	my $err = $h->{stderr};
	$h->{stderr} = '';
	$h->query_safe('ROLLBACK TO f');
	$h->query_safe("SELECT injection_points_detach('$point')");
	return $err;
}

# A table must grow: 8 rows in records and 192 in the table, which is then
# a 256-entry table 3/4 full (a slot may keep an emptied table of up to 256
# entries from an earlier transaction, so fewer rows might need no growth).
# Growing fails at a new row and at an upgrade.
foreach my $end ('ROLLBACK', 'COMMIT')
{
	my $g = $node->background_psql('postgres', on_error_stop => 0);
	$g->query_safe('BEGIN');
	$g->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM u WHERE id <= 200 ORDER BY id FOR KEY SHARE) s"
	);
	like(
		fails(
			$g, 'slog-tuple-lock-table-oom',
			'SELECT 1 FROM u WHERE id = 201 FOR UPDATE'),
		qr/out of memory/,
		"$end: growing the table for a new row fails");
	like(
		fails(
			$g, 'slog-tuple-lock-table-oom',
			'SELECT 1 FROM u WHERE id = 150 FOR UPDATE'),
		qr/out of memory/,
		"$end: growing the table for an upgrade fails");
	is(blocked('u', 1, 300, 'UPDATE'),
		200, "$end: after the failures the 200 earlier locks are all found");
	is(blocked('u', 1, 300, 'NO KEY UPDATE'),
		0, "$end: and the failed FOR UPDATE locks are not");
	$g->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM u WHERE id > 200 ORDER BY id FOR UPDATE) s"
	);
	$g->query_safe('SELECT 1 FROM u WHERE id = 150 FOR UPDATE');
	is(blocked('u', 1, 300, 'UPDATE'),
		300, "$end: the transaction goes on locking after the failures");
	is(blocked('u', 1, 300, 'NO KEY UPDATE'),
		101, "$end: its FOR UPDATE locks are found");
	$g->query_safe($end);
	is(blocked('u', 1, 300, 'UPDATE'), 0, "$end: nothing left after $end");

	# the next transaction of this backend reuses the emptied table
	$g->query_safe('BEGIN');
	$g->query_safe(
		"SELECT count(*) FROM (SELECT 1 FROM u WHERE id <= 30 ORDER BY id FOR KEY SHARE) s"
	);
	is(blocked('u', 1, 300, 'UPDATE'),
		30, "$end: the next transaction's locks are found");
	is(blocked('u', 1, 300, 'NO KEY UPDATE'),
		0, "$end: and only those, at their strength");
	$g->query_safe('COMMIT');
	is(blocked('u', 1, 300, 'UPDATE'), 0, "$end: and released");
	$g->quit;
}

# A lock that needs a new relation array and a table, whose table fails, and
# a lock whose relation array fails.  Five relations need a second array
# (the first has four entries).
my $g = $node->background_psql('postgres', on_error_stop => 0);
$g->query_safe('BEGIN');
$g->query_safe(
	"SELECT count(*) FROM (SELECT 1 FROM u WHERE id <= 8 ORDER BY id FOR UPDATE) s"
);
$g->query_safe('SELECT 1 FROM u WHERE id = 9 FOR UPDATE');
$g->query_safe("SELECT 1 FROM $_ WHERE id = 1 FOR UPDATE")
  foreach ('w1', 'w2', 'w3');
like(
	fails(
		$g, 'slog-tuple-lock-oom',
		'SELECT 1 FROM w4 WHERE id = 1 FOR UPDATE'),
	qr/out of memory/,
	'a lock whose relation array cannot grow fails');
like(
	fails(
		$g, 'slog-tuple-lock-table-oom',
		'SELECT 1 FROM w4 WHERE id = 1 FOR UPDATE'),
	qr/out of memory/,
	'a lock whose new table cannot be allocated fails');
is(blocked('w4', 1, 10, 'UPDATE'), 0, 'the failed lock is not found');
is( blocked('u', 1, 60, 'UPDATE') . ' '
	  . join(' ', map { blocked($_, 1, 10, 'UPDATE') } ('w1', 'w2', 'w3')),
	'9 1 1 1',
	'the earlier locks in five relations are all found');

# The same failure again and again: shared memory must not grow.
my $before = dsa_size();
cmp_ok($before, '>', 0, 'the lock tables exist');
$g->query_safe(
	"SELECT injection_points_attach('slog-tuple-lock-table-oom', 'notice')");
$g->query_safe(
	q{
DO $$
DECLARE n int := 0;
BEGIN
  FOR i IN 1..10000 LOOP
    BEGIN
      PERFORM 1 FROM w4 WHERE id = 1 FOR UPDATE;
    EXCEPTION WHEN out_of_memory THEN n := n + 1;
    END;
  END LOOP;
  IF n <> 10000 THEN RAISE 'only % failures', n; END IF;
END $$});
$g->query_safe("SELECT injection_points_detach('slog-tuple-lock-table-oom')");
is(dsa_size(), $before, '10000 failed allocations use no shared memory');
$g->query_safe("SELECT 1 FROM $_ WHERE id = 1 FOR UPDATE")
  foreach ('w4', 'w5');
is( join(' ',
		map { blocked($_, 1, 10, 'UPDATE') } ('w1', 'w2', 'w3', 'w4', 'w5')),
	'1 1 1 1 1',
	'then the transaction locks rows of five relations past its records');
$g->query_safe('ROLLBACK');
is( blocked('u', 1, 60, 'UPDATE') . ' '
	  . join(' ',
		map { blocked($_, 1, 10, 'UPDATE') } ('w1', 'w2', 'w3', 'w4', 'w5')),
	'0 0 0 0 0 0',
	'and nothing is left after its rollback');
$g->quit;

$node->stop;
done_testing();
