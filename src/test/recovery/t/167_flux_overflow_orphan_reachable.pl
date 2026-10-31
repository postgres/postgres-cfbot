
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX-native overflow orphan sweep must never free a reachable chunk.
#
# VACUUM's orphan sweep (FluxOverflowCheckOrphans) marks every chunk that a
# version still visible to someone can reach -- on the page, or only through
# the row's UNDO chain -- and frees the rest.  Freeing a reachable chunk is
# data loss ("FLUX overflow: root chunk lp invalid").  Two properties guard
# it here:
#
#   - The reachable set is keyed by (block, offset).  The key must compare
#     equal byte-for-byte wherever it is built; padding bytes left
#     uninitialised make a mark and a later lookup disagree, which an -O2
#     AddressSanitizer build (cfbot's m64) exposes.
#
#   - The chain walk must reverse-apply every UNDO record kind, including the
#     compact delta records an in-place UPDATE writes when its byte diff is
#     small.  A delta can change the wide column, so stopping at one would
#     leave an old value's chunks unmarked.  Updating every row while a
#     REPEATABLE READ snapshot is held produces deltas reliably.
#
# The test uses io_method=io_uring where the build supports it (as cfbot's m64
# does) and the default otherwise; neither property depends on it.  On a
# primary with a hot standby, under wal_consistency_checking = all, it checks
# that after VACUUM every wide value -- current, superseded but held by an old
# snapshot across several updates, and read on the standby -- matches a heap
# twin by md5; that the sweep frees nothing while the snapshot is held; and
# that it reclaims those chunks once the snapshot ends.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Detect io_uring support the same way src/test/modules/test_aio does: the
# invalid-enum error lists the valid io_method values.  Use -C so the check
# runs even as administrator on Windows.
my ($stdout, $stderr) =
  run_command [qw(postgres -C invalid -c io_method=invalid)];
my $have_io_uring =
  ($stderr =~ m/Available values:[^\.]*\bio_uring\b/) ? 1 : 0;
my $io_method = $have_io_uring ? 'io_uring' : 'worker';

my $primary = PostgreSQL::Test::Cluster->new('orphan_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"io_method = $io_method\n"
	  . "wal_consistency_checking = all\n"
	  . "autovacuum = off\n");
$primary->start;

# Confirm the node runs the intended I/O method (not a silent fallback).
is($primary->safe_psql('postgres', 'SHOW io_method'),
	$io_method, "primary runs with io_method=$io_method");

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('orphan_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf', "io_method = $io_method\n");
$standby->start;

# ~10 kB incompressible out-of-line value: forces FLUX-native overflow chunks.
sub big { my ($seed) = @_; return "repeat(md5($seed)::text, 320)" }

# A FLUX relation under test and a heap twin holding the identical values; the
# heap twin is the oracle the FLUX reads are compared against.
$primary->safe_psql(
	'postgres', qq{
CREATE TABLE ovf_flux (id int PRIMARY KEY, big text) USING flux;
ALTER TABLE ovf_flux ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE TABLE ovf_heap (id int PRIMARY KEY, big text) USING heap;
ALTER TABLE ovf_heap ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO ovf_flux SELECT g, @{[big('g::text')]} FROM generate_series(1, 60) g;
INSERT INTO ovf_heap SELECT g, @{[big('g::text')]} FROM generate_series(1, 60) g;
});

# md5 of the whole table's (id,big) content, detoasting every wide value.  If
# the sweep freed a reachable chunk, this read raises "root chunk lp invalid"
# (the test dies) or returns a different digest.
my $digest = q{md5(string_agg(id || ':' || big, ',' ORDER BY id))};
sub flux_digest { return $_[0]->safe_psql('postgres', "SELECT $digest FROM ovf_flux"); }
sub heap_digest { return $_[0]->safe_psql('postgres', "SELECT $digest FROM ovf_heap"); }

# Reclaimed-chunk count from a VACUUM's orphan NOTICE (0 if it emits none).
sub vacuum_reclaimed
{
	my ($node) = @_;
	my ($out, $err);
	$node->psql('postgres', 'VACUUM ovf_flux',
		stdout => \$out, stderr => \$err);
	return ($err =~ /(\d+) orphan chunk\(s\) found, (\d+) reclaimed/) ? $2 : 0;
}

# Supersede every wide value a few times so the walk must cross several UNDO
# records (a mix of delta and full before-images) and separate each live row's
# CURRENT chunks (reachable) from the superseded ones.  Keep the heap twin in
# lockstep.
for my $round (1 .. 3)
{
	my $set = "big = @{[big(\"(id*1000+$round)::text\")]}";
	$primary->safe_psql('postgres', "UPDATE ovf_flux SET $set");
	$primary->safe_psql('postgres', "UPDATE ovf_heap SET $set");
}

# Class 1: current wide value survives VACUUM, FLUX == heap twin.
$primary->safe_psql('postgres', 'VACUUM ovf_flux');
is(flux_digest($primary), heap_digest($primary),
	'current wide values read back intact after VACUUM (FLUX == heap)');

# Class 2: a snapshot taken before an UPDATE of EVERY row still reads the OLD
# wide values through the UNDO before-images, across a VACUUM.  (Updating all
# rows, not one, and detoasting in the held snapshot, is what tripped the
# delta-walk bug; updating a single row did not.)  The sweep must keep those
# superseded chunks AND not free the current reachable ones, and must free 0
# chunks while the snapshot holds them.
my $old_flux = flux_digest($primary);
my $rr = $primary->background_psql('postgres');
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$rr->query_safe("SELECT $digest FROM ovf_flux");    # pin snapshot, detoast now
$primary->safe_psql('postgres', "UPDATE ovf_flux SET big = @{[big('777777::text')]}");
$primary->safe_psql('postgres', "UPDATE ovf_heap SET big = @{[big('777777::text')]}");
is(vacuum_reclaimed($primary), 0,
	'VACUUM frees 0 chunks while an old snapshot holds the superseded values');
is($rr->query_safe("SELECT $digest FROM ovf_flux"),
	$old_flux,
	'old snapshot reads the superseded wide values across VACUUM');
$rr->query_safe('COMMIT');
$rr->quit;

# Class 2b: once the snapshot ends, VACUUM reclaims the now-unreferenced old
# chunks (reclaim still works; no leak), and the current values stay intact.
# The reclaim horizon is the global oldest xmin, which concurrent backends on
# a busy machine can hold back briefly, so retry a bounded number of times
# rather than demand reclaim on the very first VACUUM.
my $reclaimed = 0;
for (1 .. 50)
{
	$reclaimed += vacuum_reclaimed($primary);
	last if $reclaimed > 0;
	usleep(100_000);
}
ok($reclaimed > 0,
	"VACUUM reclaims the superseded chunks once the snapshot ends ($reclaimed)");
is(flux_digest($primary), heap_digest($primary),
	'current wide values intact once the old snapshot has ended');

# Class 3: the standby replays the overflow writes and the VACUUM reclaim and
# reads every current wide value back identically, under
# wal_consistency_checking = all.
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', "SELECT $digest FROM ovf_flux"),
	flux_digest($primary),
	'standby reads the current wide values back intact after replay');

ok(!$primary->log_contains('root chunk lp invalid'),
	'no "root chunk lp invalid" on the primary');
ok(!$standby->log_contains('root chunk lp invalid'),
	'no "root chunk lp invalid" on the standby');
ok(!$standby->log_contains('inconsistent page'),
	'no overflow page inconsistency under wal_consistency_checking');

$standby->stop;
$primary->stop;
done_testing();
