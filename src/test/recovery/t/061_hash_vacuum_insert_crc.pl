# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that the WAL record written by hash index VACUUM for an overflow page is
# intact when a concurrent INSERT changes the primary bucket page.
#
# hashbucketcleanup() keeps only a pin on the primary bucket page while it
# walks the overflow chain.  Before the hash WAL rework it registered that page
# (REGBUF_STANDARD | REGBUF_NO_IMAGE | REGBUF_NO_CHANGE) with the
# XLOG_HASH_DELETE record it writes for each overflow page.  With
# wal_consistency_checking enabled for the hash resource manager,
# XLogRecordAssemble() included a full image of every registered block, so it
# read the unlocked primary bucket page.  If an INSERT added a tuple to that
# page after the record's CRC was computed but before the page was copied into
# the WAL buffers, the record was written with a CRC that did not match its
# contents, and replay stopped at it with "incorrect resource manager data
# checksum".
#
# The window is a few microseconds wide, so the test widens it with an
# injection point in XLogRecordAssemble(), placed right after the CRC is
# computed.  It is attached to the VACUUM session only and matches just the
# overflow-page delete, identified by "rmgr id/xl_info/main-data length".  The
# point still fires after the fix, so the test exercises the same interleaving
# on a correct server.
#
# The assertions state the correct behavior (all WAL is valid and replays), so
# the test fails while the bug exists.

use strict;
use warnings FATAL => 'all';

use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
wal_consistency_checking = 'hash'
autovacuum = off
fsync = off
checkpoint_timeout = 1h
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

my $hash_rmid = $node->safe_psql('postgres',
	q{SELECT rm_id FROM pg_get_wal_resource_managers() WHERE rm_name = 'Hash'}
);

# Build a single bucket whose primary page stays live and whose overflow page
# is all dead, so VACUUM logs an XLOG_HASH_DELETE for the overflow page only:
# the primary page has no dead index entries, so no delete record is written
# for it, and the only consistency-checked hash DELETE in the window is the
# overflow one, logged while the primary page is held pin-only.  All rows share
# the key, so they land in one bucket; the first batch fills most of the
# primary page and stays live, the second spills onto the overflow page and is
# deleted.  Free slots are left on the primary page so the concurrent INSERT
# below lands there.
#
# hashbuild() sizes the index from estimate_rel_size() on the heap, which for a
# table that has never been vacuumed applies a floor of 10 pages.  Vacuuming
# first sets reltuples to 0, so the estimate is believed and the index starts
# with the minimum of 2 buckets.
$node->safe_psql(
	'postgres', q{
CREATE TABLE hash_t (k int, tag int) WITH (autovacuum_enabled = false);
VACUUM hash_t;
CREATE INDEX hash_i ON hash_t USING hash (k);
INSERT INTO hash_t SELECT 1, 0 FROM generate_series(1, 390);
INSERT INTO hash_t SELECT 1, 1 FROM generate_series(1, 210);
DELETE FROM hash_t WHERE tag = 1;
});

# Premise: metapage, two bucket pages, a bitmap page and one overflow page.
is( $node->safe_psql(
		'postgres',
		q{SELECT pg_relation_size('hash_i') / current_setting('block_size')::int}
	),
	'5',
	'premise: the index has a primary bucket page and one overflow page');

# Start from a checkpoint, so crash recovery replays everything below and
# pg_waldump has a record boundary to start from.
$node->safe_psql('postgres', 'CHECKPOINT');
my $start_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
my $start_walfile =
  $node->safe_psql('postgres', "SELECT pg_walfile_name('$start_lsn')");

# The VACUUM session.  The point is attached to this session only and must be
# loaded before the critical section in which it runs.
#
# Wait on the XLOG_HASH_DELETE record for the overflow page, not the one for
# the primary page.  Both are hash records (rm id $hash_rmid) with info 0x92
# (XLOG_HASH_DELETE 0x90 | XLR_CHECK_CONSISTENCY 0x02).  They differ in main
# data length: the primary-page delete carries just xl_hash_delete
# (SizeOfHashDelete, two bools = 2 bytes), while the overflow-page delete also
# appends the primary bucket page's block number (a 4-byte BlockNumber), for 6
# bytes.  Waiting on the 6-byte one parks VACUUM while it holds the primary
# bucket page pin-only, which is the page the INSERT below changes.
my $overflow_delete = "$hash_rmid/92/6";
my $vacuum = $node->background_psql('postgres');
my $vacuum_pid = $vacuum->query_safe('SELECT pg_backend_pid()');
$vacuum->query_safe(q{SELECT injection_points_set_local()});
$vacuum->query_safe(
	qq{SELECT injection_points_attach('xlog-assemble-after-crc', 'wait', '$overflow_delete')}
);
$vacuum->query_safe(
	q{SELECT injection_points_load('xlog-assemble-after-crc')});
$vacuum->query_until(
	qr/starting_vacuum/,
	q(\echo starting_vacuum
VACUUM (INDEX_CLEANUP on) hash_t;
));

# VACUUM has removed the dead entries from the primary page, released its lock
# (keeping the pin), and is now logging the removal from the overflow page: the
# CRC of that record is computed and the unlocked primary page is not yet
# copied.
$node->poll_query_until('postgres',
	"SELECT wait_event = 'xlog-assemble-after-crc' FROM pg_stat_activity WHERE pid = $vacuum_pid"
) or die "VACUUM did not reach the injection point";
note('VACUUM is between computing the CRC and copying the record');

# Now there is room in the primary page, so this INSERT changes it while VACUUM
# holds it pin-only during the overflow delete.
$node->safe_psql('postgres', q{INSERT INTO hash_t VALUES (1, 0)});

$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('xlog-assemble-after-crc')});
$vacuum->quit;

my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->poll_query_until('postgres',
	"SELECT pg_current_wal_flush_lsn() >= '$end_lsn'")
  or die "timed out waiting for WAL flush";

# Crash, so that start-up has to replay (and CRC-check) the workload.
$node->stop('immediate');

# 1. Every record in the range must have a valid CRC.  pg_waldump verifies it
# for every record it reads, whatever the --rmgr filter.  Point it at the
# specific segment file holding the range, not at the pg_wal directory: given a
# directory, pg_waldump takes the segment size from the first file readdir()
# returns, which may be a zero-filled future segment, and then fails with
# "invalid WAL segment size".
my $stderr;
my $ok = IPC::Run::run(
	[
		'pg_waldump',
		'--start' => $start_lsn,
		'--end' => $end_lsn,
		'--rmgr' => 'Hash',
		$node->data_dir . '/pg_wal/' . $start_walfile,
	],
	'>' => '/dev/null',
	'2>' => \$stderr);

ok($ok, 'pg_waldump reads the whole workload without errors');
unlike(
	$stderr,
	qr/incorrect resource manager data checksum/,
	'no WAL record has a mismatching CRC');

# 2. Crash recovery must replay it without complaint.
my $log_offset = -s $node->logfile;
$node->start;
my $log = slurp_file($node->logfile, $log_offset);
unlike(
	$log,
	qr/incorrect resource manager data checksum|inconsistent page found/,
	'crash recovery replays the workload without CRC or consistency errors');

$node->stop;
done_testing();
