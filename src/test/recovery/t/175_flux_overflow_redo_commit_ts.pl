
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A FLUX overflow-write redo must re-apply the page commit timestamp the
# primary stamped and logged in xl_flux_overflow_write.commit_ts.
#
# flux_overflow_add_chunk_logged() advances the overflow page's commit-ts word
# (FluxPageSetCommitTs) and logs the real timestamp in the
# XLOG_FLUX_OVERFLOW_WRITE record, which is registered REGBUF_STANDARD (no
# forced full-page image).  A freshly extended overflow page also gets an
# XLOG_FLUX_INIT_PAGE record, whose redo re-stamps commit-ts -- so the bug is
# invisible on first writes.  But a SECOND chunk written to an
# already-initialized overflow page carries only an OVERFLOW_WRITE record; its
# redo added the chunk but never re-applied the commit-ts.  Under
# full_page_writes = off (no FPI to paper over it) the standby page's commit-ts
# word then keeps the value the first write stamped while the primary advanced
# it: a genuine primary/standby page divergence.  flux_mask() zeroes this word,
# so wal_consistency_checking cannot catch it -- the test compares the raw page
# special area directly with pageinspect.
#
# Shape: tx1 inserts a few wide values that leave room on the overflow pages;
# CHECKPOINT (so later writes carry no FPI with full_page_writes off); tx2
# (a later transaction, larger commit timestamp) appends more wide values whose
# chunks pack onto those existing overflow pages.  After replay, the last 8
# bytes (the FluxPageOpaque commit-ts word) of every overflow-relation page
# must be identical on primary and standby.  On the unfixed redo they differ.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my (undef, $stderr) =
  run_command [qw(postgres -C invalid -c io_method=invalid)];
my $io_method =
  ($stderr =~ m/Available values:[^\.]*\bio_uring\b/) ? 'io_uring' : 'worker';

my $primary = PostgreSQL::Test::Cluster->new('ow_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"io_method = $io_method\n"
	  # no forced full-page image, so redo must reconstruct the commit-ts word
	  . "full_page_writes = off\n"
	  . "wal_consistency_checking = all\n"
	  . "autovacuum = off\n");
$primary->start;

$primary->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE ov (id int PRIMARY KEY, big text) USING flux;
ALTER TABLE ov ALTER COLUMN big SET STORAGE EXTERNAL;
-- tx1: wide values that leave free space on their overflow pages.
INSERT INTO ov SELECT g, repeat(md5(g::text), 80) FROM generate_series(1, 3) g;
});

# Base-backup now, so the standby replays tx2 from WAL (no FPI).
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('ow_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf', "io_method = $io_method\n");
$standby->start;

$primary->safe_psql('postgres', 'CHECKPOINT');

# tx2: a later transaction (larger commit timestamp) whose wide values append
# chunks onto the already-initialized overflow pages from tx1.  These writes
# carry an OVERFLOW_WRITE record with no preceding INIT_PAGE and (fpw off) no
# FPI, so redo alone must carry the advanced commit-ts.
$primary->safe_psql('postgres',
	'INSERT INTO ov SELECT g, repeat(md5(g::text), 80) FROM generate_series(4, 9) g');

$primary->wait_for_replay_catchup($standby);

# Compare the commit-ts word (last 8 bytes of each page -- the FluxPageOpaque
# special area) across the whole overflow relation.
my $nblocks = $primary->safe_psql('postgres',
	q{SELECT (pg_relation_size('ov') / current_setting('block_size')::int)::int});
my $read_words = q{
SELECT string_agg(b || ':' || encode(substr(get_raw_page('ov', b),
			current_setting('block_size')::int - 7, 8), 'hex'), ',' ORDER BY b)
FROM generate_series(0, } . ($nblocks - 1) . q{) b};

my $pri = $primary->safe_psql('postgres', $read_words);
my $std = $standby->safe_psql('postgres', $read_words);

is($std, $pri,
	'overflow page commit-ts word matches on primary and standby after redo')
  or diag("primary=$pri\nstandby=$std");

$standby->stop;
$primary->stop;
done_testing();
