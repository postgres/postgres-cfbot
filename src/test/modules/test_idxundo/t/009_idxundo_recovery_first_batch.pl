# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# A loser transaction that crash recovery defers to the logical revert worker
# must keep the WAL of its whole UNDO chain, from the chain's first batch,
# even when the recovery walk stopped before reaching that batch.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_recovery_first_batch');
$node->init;
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

# Two batches, in different WAL segments, with a checkpoint between them so
# that redo starts after the first.  The startup process does not know their
# resource manager, so recovery fails to apply them and defers the chain.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
my $before = $bg->query_safe('SELECT pg_current_wal_insert_lsn()');
$bg->query_safe('SELECT idxundo_write_failing_undo()');
my $after = $bg->query_safe('SELECT pg_current_wal_insert_lsn()');
my $first_seg = $bg->query_safe("SELECT pg_walfile_name('$before')");
$bg->query_safe('SELECT pg_switch_wal()');
$bg->query_safe('CHECKPOINT');
$bg->query_safe('SELECT idxundo_write_failing_undo()');
$bg->query_safe('SELECT pg_switch_wal()');
$node->stop('immediate');
$bg->quit;

# Recovery does not need the first segment, but walking the chain does: take
# it away, so the walk stops at the first batch without reading it.
my $seg = $node->data_dir . "/pg_wal/$first_seg";
rename($seg, "$seg.away") or die "rename $seg: $!";
$node->start;
rename("$seg.away", $seg) or die "rename $seg back: $!";

ok( $node->log_contains('stopping chain walk'),
	'the recovery walk stopped before the first batch');
is( $node->safe_psql(
		'postgres', q{SELECT count(*) FROM pg_stat_get_undo_retention()
WHERE holder = 'aborted'}),
	'1',
	'the chain was deferred to the ATM');
is( $node->safe_psql(
		'postgres',
		qq{SELECT oldest_lsn >= '$before' AND oldest_lsn < '$after'
FROM pg_stat_get_undo_retention() WHERE holder = 'aborted'}),
	't',
	'the ATM entry retains WAL from the first batch of the chain');

$node->stop;
done_testing();
