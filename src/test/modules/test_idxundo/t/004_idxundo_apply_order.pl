# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# UNDO records are applied newest first -- within a batch as well as across
# batches -- both by an aborting backend and by crash recovery.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('idxundo_order');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_idxundo'");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_idxundo');

# Returns the record numbers applied for transaction $xid, in log order.
sub applied_order
{
	my ($xid) = @_;
	my @seq = slurp_file($node->logfile) =~
	  /test_idxundo: applying UNDO record (\d+) of transaction $xid\b/g;
	return join(',', @seq);
}

# Online rollback.
my $xid = $node->safe_psql(
	'postgres', q{
BEGIN;
SELECT idxundo_write_ordered_undo(3);
SELECT idxundo_write_ordered_undo(2);
SELECT pg_current_xact_id();
ROLLBACK;
});
($xid) = $xid =~ /(\d+)\s*$/;
# The log is written asynchronously; wait until the last (oldest) of the five
# records shows up.  Records are numbered per call, so record 1 appears twice;
# wait for the second occurrence via a pattern that spans all five.
$node->wait_for_log(
	qr/(?s:test_idxundo: applying UNDO record \d+ of transaction $xid\b.*){5}/
);
is(applied_order($xid), '2,1,3,2,1', 'abort applies UNDO newest first');

# Crash recovery.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('SELECT idxundo_write_ordered_undo(3)');
$bg->query_safe('SELECT idxundo_write_ordered_undo(2)');
$xid = $bg->query_safe('SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;
is(applied_order($xid), '2,1,3,2,1',
	'crash recovery applies UNDO newest first');

# ROLLBACK TO applies the subtransaction's batch; a later abort of the
# transaction must apply each remaining record exactly once and not the
# subtransaction's again.  The values tell the batches apart.
$node->restart;
my $before = -s $node->logfile;
$xid = $node->safe_psql(
	'postgres', q{
BEGIN;
SELECT idxundo_write_ordered_undo(1);
SAVEPOINT s;
SELECT idxundo_write_ordered_undo(3);
ROLLBACK TO s;
SELECT idxundo_write_ordered_undo(2);
SELECT pg_current_xact_id();
ROLLBACK;
});
($xid) = $xid =~ /(\d+)\s*$/;
$node->wait_for_log(
	qr/(?s:test_idxundo: applying UNDO record \d+ of transaction $xid\b.*){6}/,
	$before);
sleep(1);
my @seq = slurp_file($node->logfile, $before) =~
  /test_idxundo: applying UNDO record (\d+) of transaction $xid\b/g;
is(join(',', @seq),
	'3,2,1,2,1,1',
	'ROLLBACK TO, then ROLLBACK: every record applied once, newest first');

$node->stop;
done_testing();
