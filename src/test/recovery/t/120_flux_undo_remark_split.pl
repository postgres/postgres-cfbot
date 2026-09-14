# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Rolling back a FLUX in-place UPDATE that revived an index tombstone, when
# the leaf has filled up in between, must delete-mark the entry again
# atomically.
#
# A row moved from key A to B and committed leaves a delete-marked (A, TID)
# entry.  An UPDATE moving it back to A revives that entry: clearing the mark
# shrinks the tuple by its heap TID trailer.  If the UPDATE rolls back, the
# entry is delete-marked again, which needs the trailer's room back.  Here a
# concurrent transaction fills the leaf in between, so the room is gone and
# undo has to split the leaf.  The marked tuple must replace the plain one in
# that split: a reader whose snapshot predates the A -> B UPDATE needs the
# tombstone, and must never find (A, TID) missing.
#
# Checked for an inline ROLLBACK, with such a reader looking while the undo
# action waits at the injection point nbtree-deletemark-no-room, and for a
# rollback after a crash, which the undo apply worker performs.  Each case
# checks that the split was logged as a replace split, that the row is
# reachable under B and not A, and amcheck with heapallindexed.  A final
# crash replays all of it with wal_consistency_checking.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_undo_remark_split');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_consistency_checking = all
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# fill_leaf() fills block 1 of an index on (k text) exactly, so that fewer
# than 8 bytes (one heap TID trailer) remain free and nothing splits.  A key
# of L bytes takes MAXALIGN(9 + L) + 4 bytes including its line pointer, so
# the entry sizes are 8m + 4, and the last one is sized to leave 0 or 4 bytes.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE FUNCTION fill_leaf(t regclass, idx regclass) RETURNS int
LANGUAGE plpgsql AS $f$
DECLARE gap int; s int; n int := 100000;
BEGIN
  LOOP
    SELECT upper - lower INTO gap FROM page_header(get_raw_page(idx::text, 1));
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
});

my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
  . 'SET enable_indexonlyscan = off; ';

sub nblocks
{
	my ($t) = @_;
	return $node->safe_psql('postgres',
		"SELECT pg_relation_size('${t}_k') / 8192");
}

sub lsn
{
	return $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
}

sub check_rolled_back
{
	my ($t, $what) = @_;
	is( $node->safe_psql(
			'postgres', $idx . "SELECT count(id) FROM $t WHERE k = 'b'"),
		'1',
		"$what: row reachable under its restored key");
	is( $node->safe_psql(
			'postgres', $idx . "SELECT count(id) FROM $t WHERE k = 'a'"),
		'0',
		"$what: row not reachable under the rolled-back key");
	my ($rc, $stdout, $stderr) =
	  $node->psql('postgres', "SELECT bt_index_parent_check('${t}_k', true)");
	is($rc . $stderr, '0', "$what: amcheck heapallindexed");
}

# The rollback's undo split the leaf with a replace split record, and did not
# delete the plain entry first (a DELETE_MARK record with setmark set; the
# un-marking of (B, TID) is one with setmark clear).
sub check_replace_split
{
	my ($t, $start, $what) = @_;
	is(nblocks($t), '4', "$what: undo split the leaf");
	# The rollback's records need not be flushed yet; a checkpoint does that.
	$node->safe_psql('postgres', 'CHECKPOINT');
	my $out = $node->safe_psql(
		'postgres', qq{
SELECT string_agg(record_type || ' ' || description, E'\n')
  FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn())
  WHERE resource_manager = 'Btree'});
	like(
		$out,
		qr/SPLIT_[LR] .*replace: T/,
		"$what: the entry was replaced in the split");
	like($out, qr/DELETE_MARK .*setmark: F/, "$what: (B, TID) was un-marked");
	unlike(
		$out,
		qr/DELETE_MARK .*setmark: T/,
		"$what: the entry was not deleted first");
}

# Move the row from A to B and commit, revive (A, TID) in an open transaction,
# then fill the leaf from another session.  Returns the open session.
sub revive_and_fill
{
	my ($t) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int, k text) USING flux;
CREATE INDEX ${t}_k ON $t (k) WITH (deduplicate_items = off);
INSERT INTO $t VALUES (1, 'a');
});
	my $reader = $node->background_psql('postgres');
	$reader->query_safe($idx);
	$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($reader->query_safe("SELECT count(id) FROM $t WHERE k = 'a'"),
		'1', "$t: old reader sees the row under A");
	$node->safe_psql('postgres', "UPDATE $t SET k = 'b' WHERE id = 1");

	my $bg = $node->background_psql('postgres');
	$bg->query_safe("BEGIN; UPDATE $t SET k = 'a' WHERE id = 1");
	cmp_ok($node->safe_psql('postgres', "SELECT fill_leaf('$t', '${t}_k')"),
		'<', 8, "$t: leaf is full");
	is(nblocks($t), '2', "$t: filling did not split the leaf");
	return ($bg, $reader);
}

# Inline ROLLBACK.  The rolling-back session waits right after re-marking
# (A, TID), while the old reader looks it up.
my ($bg, $reader) = revive_and_fill('inline_one');
my $start = lsn();
$bg->query_safe(
	q{SELECT injection_points_set_local();
	  SELECT injection_points_attach('nbtree-deletemark-no-room', 'wait')});
$bg->query_until(
	qr/start/, q{
\echo start
ROLLBACK;
});
$node->wait_for_event('client backend', 'nbtree-deletemark-no-room');
is($reader->query_safe("SELECT count(id) FROM inline_one WHERE k = 'a'"),
	'1', 'inline rollback: old reader sees the row under A during undo');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('nbtree-deletemark-no-room')");
$bg->query_safe(
	"SELECT injection_points_detach('nbtree-deletemark-no-room')");
$bg->quit;
is($reader->query_safe("SELECT count(id) FROM inline_one WHERE k = 'a'"),
	'1', 'inline rollback: old reader sees the row under A after undo');
$reader->query_safe('COMMIT');
$reader->quit;
check_replace_split('inline_one', $start, 'inline rollback');
check_rolled_back('inline_one', 'inline rollback');

# Crash with the transaction open; the undo apply worker rolls it back.
($bg, $reader) = revive_and_fill('crash_one');
$node->safe_psql('postgres', 'CHECKPOINT');
$start = lsn();
$node->stop('immediate');
$bg->{run}->kill_kill;
$reader->{run}->kill_kill;
$node->start;
ok( $node->poll_query_until(
		'postgres', "SELECT k = 'b' FROM crash_one WHERE id = 1"),
	'crash: row rolled back');
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
	  WHERE backend_type = 'undo apply worker'}),
	'crash: undo apply worker finished');
check_replace_split('crash_one', $start, 'crash rollback');
check_rolled_back('crash_one', 'crash rollback');

# Replay both rollbacks' splits; wal_consistency_checking compares the
# replayed pages against the logged images.
$node->stop('immediate');
$node->start;
check_rolled_back('inline_one', 'inline rollback after replay');
check_rolled_back('crash_one', 'crash rollback after replay');
ok(!$node->log_contains('inconsistent page'), 'no inconsistent page in redo');

$node->stop;
done_testing();
