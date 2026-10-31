# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Crash-recovery test for nbtree delete-marking:
# delete-mark a leaf entry, crash (immediate stop, no clean shutdown),
# restart so WAL redo replays XLOG_BTREE_DELETE_MARK, and verify the entry
# is still delete-marked with an intact heap TID and passes amcheck.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('dm_recovery');
$node->init;
# Deterministic redo verification for the new WAL record type.
$node->append_conf('postgresql.conf', 'wal_consistency_checking = btree');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_deletemark');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node->safe_psql('postgres', <<'SQL');
CREATE TABLE dm_r (k int, v int);
INSERT INTO dm_r SELECT g, g FROM generate_series(1, 30) g;
CREATE INDEX dm_ridx ON dm_r (k);
SQL

# Locate a concrete heap TID and delete-mark its index entry.
my $tid = $node->safe_psql('postgres', "SELECT ctid FROM dm_r WHERE k = 15");
is( $node->safe_psql(
		'postgres', "SELECT dm_mark('dm_ridx'::regclass, '$tid'::tid)"),
	't',
	'entry delete-marked before crash');
is( $node->safe_psql(
		'postgres', "SELECT dm_classify('dm_ridx'::regclass, '$tid'::tid)"),
	'delete-marked',
	'classified delete-marked before crash');

# Make sure the mark's WAL is flushed, then crash without a clean shutdown so
# recovery must replay it.
$node->safe_psql('postgres', "SELECT pg_switch_wal()");
$node->stop('immediate');
$node->start;

# After crash recovery the bit must be re-set (WAL redo), TID intact.
is( $node->safe_psql(
		'postgres', "SELECT dm_classify('dm_ridx'::regclass, '$tid'::tid)"),
	'delete-marked',
	'entry still delete-marked after crash recovery');
is( $node->safe_psql(
		'postgres',
		"SELECT dm_heaptid('dm_ridx'::regclass, '$tid'::tid) = '$tid'::tid"),
	't',
	'heap TID intact after crash recovery');

# amcheck must be clean on the recovered index.
is($node->safe_psql('postgres', "SELECT bt_index_check('dm_ridx')"),
	'', 'bt_index_check clean after crash recovery');
is($node->safe_psql('postgres', "SELECT bt_index_parent_check('dm_ridx')"),
	'', 'bt_index_parent_check clean after crash recovery');

$node->stop;
done_testing();
