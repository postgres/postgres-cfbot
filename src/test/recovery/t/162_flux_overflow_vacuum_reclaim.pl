
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX-native overflow VACUUM reclaim: correctness without heap-TOAST-shape
# assumptions.  Wide values on a non-logically-logged FLUX relation are stored
# in FLUX-native overflow (data + directory chunks on FLUX pages), not in the
# heap TOAST table, so this test checks only CONTENT and row identity -- never
# a heap-TOAST chunk count.  It exercises the three defects the reclaim path
# had:
#
#   A (corruption): the orphan sweep freed chunks with PageIndexTupleDelete,
#       which renumbers every surviving line pointer on the page.  Overflow
#       chunks share FLUX data pages with regular tuples (indexed by TID) and
#       with other values' chunks (named by offset in directories), so a
#       VACUUM that reclaimed a superseded value shifted a live row's TID or a
#       live chunk's offset -- "invalid index offnum", an ItemIdHasStorage
#       crash, or later a wrong index lookup / "root chunk lp invalid" read.
#
#   B (MVCC lifetime): a superseded wide value's old chunks were reclaimed by
#       the creating-XID horizon, which does not protect a snapshot taken
#       before the UPDATE that still reaches the old value through the UNDO
#       before-image.
#
#   D (WAL/standby): the reclaim was unlogged, so a standby and crash recovery
#       diverged from the primary.
#
# Replay runs on a streaming standby and in crash recovery with
# wal_consistency_checking = all.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('ovr_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"wal_consistency_checking = all\nautovacuum = off\n");
$primary->start;
$primary->backup('bkp');

my $standby = PostgreSQL::Test::Cluster->new('ovr_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

# ~10 kB incompressible value, stored out of line.
sub big { my ($seed) = @_; return "repeat(md5($seed)::text, 320)" }

$primary->safe_psql(
	'postgres', qq{
CREATE TABLE ovr (id int PRIMARY KEY, n int, big text) USING flux;
ALTER TABLE ovr ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO ovr SELECT g, 0, @{[big('g::text')]} FROM generate_series(1, 20) g;
});

# --- Class A: VACUUM after many supersedes of several rows must not corrupt
# the page line-pointer index or the PK index.  Rewrite the wide column of
# every row five times (each UPDATE supersedes the old value, orphaning its
# chunks), then VACUUM.  The VACUUM must succeed and leave every row reachable
# by its key with its current value intact.
for my $round (1 .. 5)
{
	$primary->safe_psql('postgres',
		"UPDATE ovr SET big = @{[big(\"(id*100+$round)::text\")]}, n = n + 1");
}
is( $primary->safe_psql('postgres', 'VACUUM ovr; SELECT 1'),
	'1', 'Class A: VACUUM of many superseded wide values succeeds');

# Every row still reachable by its PK and holds its latest value (n = 5).
is( $primary->safe_psql(
		'postgres', q{
SELECT count(*), count(*) FILTER (WHERE n = 5
	AND big = repeat(md5((id*100+5)::text)::text, 320))
FROM ovr}),
	'20|20',
	'Class A: all 20 rows reachable with current value after VACUUM');

# A deletion of specific keys must still find them (TID/offset stability).
is( $primary->safe_psql(
		'postgres',
		'DELETE FROM ovr WHERE id IN (3, 7, 15); SELECT count(*) FROM ovr'),
	'17', 'Class A: keyed DELETE after reclaim finds its rows');
$primary->safe_psql('postgres', 'VACUUM ovr');
is( $primary->safe_psql(
		'postgres', q{
SELECT count(*), bool_and(big = repeat(md5((id*100+5)::text)::text, 320))
FROM ovr}),
	'17|t', 'Class A: 17 rows with intact values after second VACUUM');

# --- Class B: a snapshot taken before an UPDATE must still detoast the OLD
# wide value through the before-image, across a VACUUM that would otherwise
# reclaim its chunks; and the old value must be reclaimed once that snapshot
# ends.
my $old = q{repeat(md5((1*100+5)::text)::text, 320)};

my $rr = $primary->background_psql('postgres');
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$rr->query_safe('SELECT count(*) FROM ovr');    # take the snapshot
$primary->safe_psql('postgres',
	"UPDATE ovr SET big = @{[big('99999::text')]} WHERE id = 1");
$primary->safe_psql('postgres', 'VACUUM ovr');    # must NOT reclaim id=1's old chunks
is( $rr->query_safe("SELECT big = $old FROM ovr WHERE id = 1"),
	't',
	'Class B: old snapshot reads the superseded wide value across VACUUM');
$rr->query_safe('COMMIT');
$rr->quit;
$primary->safe_psql('postgres', 'VACUUM ovr');    # now reclaimable
is( $primary->safe_psql(
		'postgres',
		"SELECT big = @{[big('99999::text')]} FROM ovr WHERE id = 1"),
	't', 'Class B: current value intact once the old snapshot has ended');

# --- Class D: the standby replays every overflow write and reclaim, and crash
# recovery does too, both under wal_consistency_checking = all.  Compare a
# content digest of the whole table.
my $digest = q{SELECT count(*) || ' ' ||
	md5(string_agg(id || ':' || n || ':' || big, ',' ORDER BY id)) FROM ovr};
my $want = $primary->safe_psql('postgres', $digest);

$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', $digest),
	$want, 'Class D: standby matches the primary after overflow VACUUM replay');

# Crash-restart the primary and the standby; replay must reproduce the table.
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $digest),
	$want, 'Class D: primary unchanged after crash recovery');

$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', $digest),
	$want, 'Class D: standby unchanged after its own crash recovery');

ok(!$primary->log_contains('PANIC'), 'Class D: primary replayed without PANIC');
ok(!$standby->log_contains('PANIC'), 'Class D: standby replayed without PANIC');
ok( !$standby->log_contains('inconsistent page'),
	'Class D: no overflow page inconsistency under wal_consistency_checking');

$standby->stop;
$primary->stop;
done_testing();
