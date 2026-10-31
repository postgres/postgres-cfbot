
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A live primary-key entry must never name a FLUX-native overflow CHUNK.
#
# The bug (seen first by a bench Phase-3 release run, then reproduced here):
# under 32 concurrent INSERT ... ON CONFLICT DO NOTHING loaders of ~32 KB
# incompressible values into a FLUX table, a LIVE pkey leaf entry ends up
# naming a heap TID whose line-pointer slot holds a FLUX overflow data chunk
# (header magic "ROVC" = 0x524F5643), and the row that entry should name does
# not exist.  An index scan then reads the chunk magic as t_xmin and errors
# with "could not open file pg_xact/0524 / commit status of transaction
# 1380931139" (1380931139 = 0x524F5643).
#
# ROOT CAUSE (pinned with pg_waldump before any checkpoint): two backends
# extended the FLUX relation to the SAME new block.  The speculative-insert
# fresh-page fallback and the overflow chunk writer extended via the racy
# ReadBuffer(rel, P_NEW) -- which grows the file WITHOUT the relation-extension
# lock -- so a concurrent ExtendBufferedRel() (used by FluxGetPageWithFreeSpace)
# handed back the very same new block.  The chunk writer re-initialised and
# overwrote the speculative tuple's slot with a data chunk; the speculative
# insert then inserted a pkey entry naming that now-foreign chunk slot.
# No VACUUM, no slot recycling and no crash are involved.
#
# The test drives the concurrent ON CONFLICT wide-insert load continuously in
# the background and, after each short burst, runs a deterministic,
# buffer-independent amcheck-style check in SQL: for every LIVE pkey leaf
# entry, the heap TID it names must hold a normal user tuple, never an overflow
# chunk (ROVC) and never a non-normal line pointer; and the index-ordered row
# count must equal the sequential-scan count and must not error on a chunk's
# magic.  Because the extension race is probabilistic, the load is repeated
# (truncate + reload) for as many bursts as fit under a hard time cap; on the
# unfixed tree a burst reproduces the orphan, so this is RED.  With the
# race-safe extension it never reproduces -> GREEN.  It adds no injection
# points and drives only ordinary SQL, so it is valid for any fix that closes
# the extension race.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start);
use Time::HiRes qw(time);

# ---------------------------------------------------------------------------
# Parameters (calibrated RED on the cassert debug build within the time cap).
# The race fires most when many backends extend an initially-empty relation at
# once, so each burst starts from an empty table and extends hard.
# ---------------------------------------------------------------------------
my $CLIENTS   = 32;     # concurrent ON CONFLICT loaders (brief: 32)
my $THREADS   = 32;
my $RANGE     = 1500;   # ids per client; random draw -> real within-client conflicts
                        # (lowered below for a small-segment build)
my $WIDE_BLKS = 1024;   # md5 blocks -> ~32 KB incompressible => FLUX-native overflow
my $HARD_CAP  = 300;    # no new burst after this; slow (sanitizer) builds stay
                        # well inside meson's 1000 s timeout
my $MAX_BURSTS = 14;

my $node = PostgreSQL::Test::Cluster->new('flux_pkey_names_chunk');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
max_connections = 100
fsync = off
synchronous_commit = off
# small cache forces overflow-chunk pages to be evicted and the relation to be
# extended under contention, which is when the extension race fires.
shared_buffers = 128MB
bgwriter_delay = 10ms
});
$node->start;

my $DB = 'postgres';

# A build with tiny segments (--with-segsize-blocks, as in CI's autoconf task)
# spreads each burst's ~1 GB over some 20,000 segment files, and a burst then
# takes several minutes instead of seconds.  Run smaller bursts there, so the
# test fits its time budget; every other build keeps the burst size this test
# was calibrated with.
$RANGE = 300
  if $node->safe_psql($DB,
	"SELECT pg_size_bytes(current_setting('segment_size')) < 64 * 1024 * 1024")
  eq 't';

# mkbig(blocks, seed): ~32*blocks bytes of incompressible md5-hex text, so wide
# values take the FLUX-native overflow path (chunks on FLUX data pages), not
# inline compression or heap TOAST.
$node->safe_psql(
	$DB, qq{
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE OR REPLACE FUNCTION mkbig(blocks int, seed text) RETURNS text
LANGUAGE sql IMMUTABLE AS \$fn\$
  SELECT string_agg(md5(seed || g::text), '') FROM generate_series(1, blocks) g
\$fn\$;
CREATE TABLE ovc (id int PRIMARY KEY, v text) USING flux;
});

# pgbench script: each client draws a random id in its own range, so the same
# id recurs within a client and ON CONFLICT DO NOTHING hits a real conflict
# (which drives the speculative-insert / overflow-writer interleaving).
my $script = $PostgreSQL::Test::Utils::tmp_check . '/170_load.sql';
append_to_file(
	$script, qq{
\\set i random(1, $RANGE)
\\set id :client_id * $RANGE + :i
INSERT INTO ovc VALUES (:id, mkbig($WIDE_BLKS, 'c' || (:id)::text)) ON CONFLICT DO NOTHING;
});

# Deterministic, buffer-independent detector.  Returns the number of LIVE pkey
# leaf entries whose named heap TID is NOT a normal user tuple -- i.e. holds a
# FLUX overflow chunk (first 4 bytes "ROVC") or a non-normal line pointer.
# Reads the index leaves and the heap page items directly, so it does not
# depend on buffer-cache state or on an index scan erroring.
my $detector = q{
SELECT count(*) FROM (
  SELECT (bt.ctid::text::point)[0]::int AS blk,
         (bt.ctid::text::point)[1]::int AS off
  FROM generate_series(1,(pg_relation_size('ovc_pkey')/8192)::int-1) l(blk),
       LATERAL bt_page_items('ovc_pkey', l.blk) bt
  WHERE bt.ctid IS NOT NULL AND NOT bt.dead) e
JOIN LATERAL (
  SELECT encode(substring(get_raw_page('ovc', e.blk) FROM hpi.lp_off+1 FOR 4),
                'hex') AS magic, hpi.lp_flags
  FROM heap_page_items(get_raw_page('ovc', e.blk)) hpi
  WHERE hpi.lp = e.off) m ON true
WHERE m.magic = '43564f52' OR m.lp_flags <> 1};

my $start   = time();
my $orphans = 0;
my $burst   = 0;
my $detail  = '';

while ($burst < $MAX_BURSTS && time() - $start < $HARD_CAP)
{
	$burst++;
	$node->safe_psql($DB, 'TRUNCATE ovc');

	# One burst: the concurrent load, extending an initially-empty relation.
	my ($out, $err) = ('', '');
	my $h = start(
		[
			'pgbench', '--no-vacuum', '-M' => 'prepared',
			'--client' => $CLIENTS, '--jobs' => $THREADS,
			'--transactions' => $RANGE,
			'--file' => $script,
			$node->connstr($DB),
		],
		'>' => \$out, '2>' => \$err);
	$h->finish;

	# Fast per-burst check (cheap, deterministic): an index scan over every
	# wide value.  If any live pkey entry names an overflow chunk, the fetch
	# reads the chunk's ROVC magic as t_xmin and errors on the CLOG lookup, so
	# psql returns non-zero.  Also require the index-ordered count to equal the
	# seqscan count (a non-erroring orphan, e.g. a non-normal slot, shows up as
	# a count mismatch).
	my $seq = $node->safe_psql($DB,
		'SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM ovc');
	my $idx = '';
	my $ierr = $node->psql($DB,
		'SET enable_seqscan=off; SET enable_bitmapscan=off; SET max_parallel_workers_per_gather=0; SELECT count(v) FROM ovc',
		stdout => \$idx);

	# Structural backstop (buffer-independent, exact): count LIVE pkey entries
	# whose named slot is an overflow chunk or a non-normal line pointer.  Run
	# only when the fast check already flagged trouble, because it does a heap
	# lookup per index entry and is slow.
	if ($ierr != 0 || (defined $idx && $idx ne $seq))
	{
		$orphans = $node->safe_psql($DB, $detector) + 0;
	}

	note "burst $burst: orphan_entries=$orphans seq=$seq idx=$idx idx_rc=$ierr "
	  . "elapsed=" . sprintf('%.0f', time() - $start) . "s";

	if ($orphans > 0 || $ierr != 0 || (defined $idx && $idx ne $seq))
	{
		$detail = "burst $burst: orphans=$orphans seq=$seq idx=$idx idx_rc=$ierr";
		last;
	}
}

# ---------------------------------------------------------------------------
# The assertion.  On the unfixed tree a burst produces a live pkey entry that
# names an overflow chunk (and the index scan errors on its ROVC magic), so
# this FAILS.  With the race-safe relation extension no such entry ever forms
# and every burst is clean -> this PASSES.
# ---------------------------------------------------------------------------
ok( $orphans == 0 && $detail eq '',
	'no live FLUX pkey entry names an overflow chunk after concurrent ON CONFLICT wide inserts'
) or diag("orphan pkey entry naming a chunk reproduced: $detail");

$node->stop('fast');
done_testing();
