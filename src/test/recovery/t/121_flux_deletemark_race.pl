# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Delete-marking an nbtree entry that has no room to grow on its leaf must be
# atomic.  A FLUX in-place UPDATE that changes an indexed key delete-marks the
# old (k_old, TID) entry, which grows it by a heap TID trailer.  When the leaf
# is full the entry cannot grow in place.  A reader whose snapshot predates
# the UPDATE still has to reach the row under k_old the whole time; it must
# never find the entry missing.
#
# The injection point nbtree-deletemark-no-room fires in btdeletemark's
# no-room path once the leaf lock has been released.  The UPDATE waits there
# while the old-snapshot reader looks the row up under k_old.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_deletemark_race');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# fill_leaf() fills leaf block blk of an index on (k text) until fewer than
# 8 bytes (one heap TID trailer) remain free.  A key of L bytes takes
# MAXALIGN(9 + L) + 4 bytes including its line pointer, so the entry sizes
# are 8m + 4, and the last one is sized to leave 0 or 4 bytes.  The keys
# start with 'c', so they land on the leftmost leaf.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE FUNCTION fill_leaf(t regclass, idx regclass, blk int) RETURNS int
LANGUAGE plpgsql AS $f$
DECLARE gap int; s int; n int := 100000;
BEGIN
  LOOP
    SELECT upper - lower INTO gap FROM page_header(get_raw_page(idx::text, blk));
    EXIT WHEN gap < 8;
    IF gap >= 40 THEN s := 20;
    ELSIF gap % 8 = 4 THEN s := gap;
    ELSE s := gap - 4;
    END IF;
    n := n + 1;
    EXECUTE format('INSERT INTO %s VALUES ($1, $2)', t)
      USING n, 'c' || lpad(n::text, s - 14, '0');
  END LOOP;
  RETURN gap;
END $f$;
CREATE FUNCTION leftmost_leaf(idx regclass) RETURNS int
LANGUAGE sql AS $f$
  SELECT s.blkno::int FROM generate_series(1,
      pg_relation_size(idx) / 8192 - 1) AS g(b),
    LATERAL bt_page_stats(idx::text, g.b::int) s
  WHERE s.type = 'l' AND s.btpo_prev = 0
$f$;

});

my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
  . 'SET enable_indexonlyscan = off; ';
my @lsns;

# Fill the leftmost leaf of $t's index and delete-mark one of its entries:
# the first one ('first'), which the split leaves on the left half, or the
# last one ('last'), which goes to the right half.
sub run_case
{
	my ($t, $which) = @_;

	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, k text) USING flux;
CREATE INDEX ${t}_k ON $t (k) WITH (deduplicate_items = off);
INSERT INTO $t VALUES (1, 'a');
INSERT INTO $t SELECT i, 'm' || lpad(i::text, 30, '0')
  FROM generate_series(2, 1000) i;
});

	my $leaf = $node->safe_psql('postgres', "SELECT leftmost_leaf('${t}_k')");
	cmp_ok(
		$node->safe_psql(
			'postgres', "SELECT fill_leaf('$t', '${t}_k', $leaf)"),
		'<', 8,
		"$t: leftmost leaf is full");
	is($node->safe_psql('postgres', "SELECT leftmost_leaf('${t}_k')"),
		$leaf, "$t: filling did not split the leaf");

	my $id = 1;
	$id = $node->safe_psql(
		'postgres', qq{
SELECT id FROM $t WHERE ctid = (SELECT htid FROM bt_page_items('${t}_k', $leaf)
  ORDER BY itemoffset DESC LIMIT 1)}) if $which eq 'last';
	my $kold =
	  $node->safe_psql('postgres', "SELECT k FROM $t WHERE id = $id");

	my $q = "SELECT count(id) FROM $t WHERE k = '$kold'";

	# The reader takes its snapshot before the UPDATE.
	my $reader = $node->background_psql('postgres');
	$reader->query_safe($idx);
	$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($reader->query_safe($q), '1', "$t: reader sees the row under k_old");

	push @lsns,
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

	# The UPDATE moves the row to the rightmost leaf, then delete-marks
	# (k_old, TID) on the full leftmost leaf and waits in the no-room path.
	my $updater = $node->background_psql('postgres');
	$updater->query_safe(
		q{SELECT injection_points_set_local();
		  SELECT injection_points_attach('nbtree-deletemark-no-room', 'wait')});
	$updater->query_until(
		qr/start/, qq{
\\echo start
UPDATE $t SET k = 'zzz' WHERE id = $id;
});
	$node->wait_for_event('client backend', 'nbtree-deletemark-no-room');

	is($reader->query_safe($q),
		'1',
		"$t: reader still sees the row under k_old during the delete-mark");

	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('nbtree-deletemark-no-room')");
	$updater->query_safe(
		"SELECT injection_points_detach('nbtree-deletemark-no-room')");
	$updater->quit;

	push @lsns,
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

	is($reader->query_safe($q),
		'1', "$t: reader sees the row under k_old after the UPDATE");
	$reader->query_safe('COMMIT');
	$reader->quit;

	check_index($t, $kold, 'after the UPDATE');
	return $kold;
}

sub check_index
{
	my ($t, $kold, $what) = @_;

	is( $node->safe_psql(
			'postgres', $idx . "SELECT count(id) FROM $t WHERE k = '$kold'"),
		'0',
		"$t $what: new snapshot does not see the row under k_old");
	is( $node->safe_psql(
			'postgres', $idx . "SELECT count(id) FROM $t WHERE k = 'zzz'"),
		'1',
		"$t $what: new snapshot sees the row under k_new");
	my ($rc, $stdout, $stderr) =
	  $node->psql('postgres', "SELECT bt_index_parent_check('${t}_k', true)");
	is($rc . $stderr, '0', "$t $what: amcheck heapallindexed");
}

my $kleft = run_case('t_left', 'first');
my $kright = run_case('t_right', 'last');

# Each delete-mark was logged as one split record that replaces the entry.
my $wal = $node->data_dir . '/pg_wal';
my ($out, $err) = run_command(
	[
		'pg_waldump', '-p', $wal, '-s', $lsns[0], '-e',
		$lsns[1], '-r', 'Btree'
	]);
like(
	$out,
	qr/SPLIT_L .*replace: T/,
	't_left: replace split logged as SPLIT_L');
($out, $err) = run_command(
	[
		'pg_waldump', '-p', $wal, '-s', $lsns[2], '-e',
		$lsns[3], '-r', 'Btree'
	]);
like(
	$out,
	qr/SPLIT_R .*replace: T/,
	't_right: replace split logged as SPLIT_R');

# Many key UPDATEs against an index built with full leaves.  The new keys go
# to the right end of the index, so the old leaves stay full and the
# delete-marks there need replace splits.  The parent level splits as well.  A reader
# whose snapshot predates them must still find every row by its old key.
$node->safe_psql(
	'postgres', q{
CREATE TABLE s (id int, k text) USING flux;
INSERT INTO s SELECT i, md5(i::text) FROM generate_series(1, 20000) i;
CREATE INDEX s_k ON s (k) WITH (deduplicate_items = off, fillfactor = 100);
});
my $reader = $node->background_psql('postgres');
$reader->query_safe($idx);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $allold = q{SELECT count(id) FROM s WHERE k = md5(id::text)};
is($reader->query_safe($allold), '20000', 's: reader sees every row');
my $start =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', "UPDATE s SET k = 'z' || k WHERE id % 4 = $_")
  for (0 .. 2);
cmp_ok(
	$node->safe_psql(
		'postgres', qq{
SELECT count(*) FROM pg_get_wal_records_info('$start', pg_current_wal_insert_lsn())
  WHERE resource_manager = 'Btree' AND description LIKE '%replace: T%'}),
	'>',
	50,
	's: delete-marks were done with replace splits');
cmp_ok(
	$node->safe_psql(
		'postgres', qq{
SELECT count(*) FROM pg_get_wal_records_info('$start', pg_current_wal_insert_lsn())
  WHERE resource_manager = 'Btree' AND record_type LIKE 'SPLIT%'
    AND description LIKE 'level: 1,%'}),
	'>',
	0,
	's: the parent level split, too');
is($reader->query_safe($allold),
	'20000', 's: reader still sees every row under its old key');
$reader->query_safe('COMMIT');
$reader->quit;
my ($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_parent_check('s_k', true)");
is($rc . $stderr, '0', 's: amcheck heapallindexed');

# Crash recovery replays the splits (with wal_consistency_checking, it also
# compares the result against the logged page images).
$node->stop('immediate');
$node->start;
check_index('t_left', $kleft, 'after crash recovery');
check_index('t_right', $kright, 'after crash recovery');
is( $node->safe_psql(
		'postgres', $idx . 'SELECT count(id) FROM s WHERE k = md5(id::text)'),
	'5000',
	's after crash recovery: rows not updated keep their key');
($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_parent_check('s_k', true)");
is($rc . $stderr, '0', 's after crash recovery: amcheck heapallindexed');

$node->stop;
done_testing();
