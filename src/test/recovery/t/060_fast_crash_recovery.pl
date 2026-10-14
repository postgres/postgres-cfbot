# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery with on-demand WAL replay (fast_crash_recovery): the server
# accepts connections once the WAL has been scanned, each page is recovered
# when it is first read, and a background worker recovers the rest.  The
# data must be right at every stage: right after the server opens, after the
# worker has drained the index, and after a clean restart.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
fast_crash_recovery = on
checkpoint_timeout = 1d
max_wal_size = 10GB
log_checkpoints = on
));
$node->start;

# A table with an index, checkpointed, then modified so that the changes
# exist only in WAL when we crash: updates, deletes, inserts, and a table
# that is created and dropped inside the window.
#
# A second table is truncated by VACUUM inside the window.  The truncation
# record has no block reference, so the startup process replays it during
# the scan, and doing so reads the last visibility map and free space map
# page into shared buffers to clear their tails.  The later changes to
# all-visible pages reference that visibility map page, so they test what
# happens to records for a page that is resident during the scan; and the
# table grows past the truncation point again, so a truncation replayed out
# of order would lose those rows.
$node->safe_psql(
	'postgres', q(
CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL, pad text);
INSERT INTO t SELECT g, 0, repeat('x', 200) FROM generate_series(1, 20000) g;
CREATE TABLE shrunk (id int PRIMARY KEY, v int NOT NULL, pad text);
INSERT INTO shrunk SELECT g, 0, repeat('y', 500) FROM generate_series(1, 4000) g;
VACUUM shrunk;
CHECKPOINT;
UPDATE t SET v = v + 1 WHERE id % 3 = 0;
DELETE FROM t WHERE id % 10 = 0;
INSERT INTO t SELECT g, -1, '' FROM generate_series(20001, 25000) g;
CREATE TABLE dropped AS SELECT g FROM generate_series(1, 5000) g;
DROP TABLE dropped;
VACUUM t;
DELETE FROM shrunk WHERE id > 2000;
VACUUM shrunk;
UPDATE shrunk SET v = 1 WHERE id IN (1, 500, 1000, 1999);
INSERT INTO shrunk SELECT g, 2, '' FROM generate_series(2001, 2500) g;
));

my $seqscan = 'SELECT count(*), sum(v), sum(length(pad)) FROM t';
my $indexscan =
  'SET enable_seqscan = off; SET enable_bitmapscan = off; '
  . 'SELECT count(*), sum(v) FROM t WHERE id > 0';
my $shrunkscan =
  "SELECT count(*), sum(v), pg_relation_size('shrunk') FROM shrunk";
my $expected_seq = $node->safe_psql('postgres', $seqscan);
my $expected_idx = $node->safe_psql('postgres', $indexscan);
my $expected_shrunk = $node->safe_psql('postgres', $shrunkscan);

# Crash.  Log at DEBUG1 from here on, to see the scan evict the pages the
# truncation read.
$node->append_conf('postgresql.conf', 'log_min_messages = debug1');
$node->stop('immediate');
my $log_offset = -s $node->logfile;
$node->start;

ok( $node->log_contains('fast recovery worker started', $log_offset),
	'crash recovery used on-demand WAL replay');
ok( $node->log_contains(
		qr/fast recovery: evicted block \d+ of relation \S+ fork \d+ to index its records/,
		$log_offset),
	'a page read during the scan was evicted to index its later records');

# 1. Right after the server opened: pages are replayed as they are read, by
# a sequential scan and by an index scan.
is($node->safe_psql('postgres', $seqscan),
	$expected_seq, 'data correct right after the server opened (seq scan)');
is($node->safe_psql('postgres', $indexscan),
	$expected_idx, 'data correct right after the server opened (index scan)');
is($node->safe_psql('postgres', $shrunkscan),
	$expected_shrunk,
	'truncated-and-regrown table correct right after the server opened');

# 2. After the worker has recovered every remaining page.
$node->wait_for_log(qr/fast crash recovery complete/, $log_offset);
is($node->safe_psql('postgres', $seqscan),
	$expected_seq, 'data correct after the worker drained the index');
is($node->safe_psql('postgres', $shrunkscan),
	$expected_shrunk,
	'truncated-and-regrown table correct after the worker drained the index');

# Checkpoints are allowed again once the index is gone.
$node->safe_psql('postgres', 'CHECKPOINT');
ok( $node->log_contains('checkpoint complete', $log_offset),
	'a checkpoint ran after the index was drained');

# 3. After a clean restart, which must not need recovery at all.
$log_offset = -s $node->logfile;
$node->restart;
ok( !$node->log_contains('automatic recovery in progress', $log_offset),
	'clean restart needed no recovery');
is($node->safe_psql('postgres', $seqscan),
	$expected_seq, 'data correct after a clean restart');
is($node->safe_psql('postgres', $shrunkscan),
	$expected_shrunk, 'truncated-and-regrown table correct after a clean restart');

# The dropped table's pages were forgotten, not resurrected.
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM pg_class WHERE relname = 'dropped'"),
	'0',
	'table dropped inside the window stays dropped');

$node->stop;
done_testing();
