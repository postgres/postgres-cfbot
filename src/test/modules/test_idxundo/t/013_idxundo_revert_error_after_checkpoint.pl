# Copyright (c) 2026, PostgreSQL Global Development Group

# An error while the logical revert worker reverts an ATM entry is retried,
# however old the entry is.  A checkpoint whose redo pointer passes the
# entry's UNDO does not remove that WAL: KeepLogSeg retains it for as long
# as the entry exists.  Only a removed segment means the rollback can no
# longer be completed.
#
# First the error alone, then the same error after a CHECKPOINT: both must
# leave the server up, log the retry, and revert the entry once the error
# goes away.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_error_ckpt');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
max_logical_revert_workers = 2
logical_revert_naptime = 100ms
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_idxundo;
CREATE EXTENSION amcheck;
CREATE TABLE warm (id int primary key) WITH (index_undo = on);
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

# Get a revert worker running for this database: the launcher starts one at
# startup for a database with ATM entries, and the worker then stays.
$node->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO warm SELECT g FROM generate_series(1, 5000) g;
ROLLBACK;
});
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM idxundo_atm_entries()'),
	'revert worker is running');

# A large abort, which goes to the ATM; returns its xid.
sub abort_into_atm
{
	my ($from) = @_;
	my $to = $from + 4999;
	my $out = $node->safe_psql(
		'postgres', qq{
BEGIN;
INSERT INTO t SELECT g FROM generate_series($from, $to) g;
SELECT pg_current_xact_id();
ROLLBACK;
});
	my ($xid) = $out =~ /(\d+)\s*$/;
	return $xid;
}

# The error is logged as a retry, the server stays up, and once the error
# point is detached the entry is reverted.
sub check_retried
{
	my ($xid, $offset, $what) = @_;

	$node->wait_for_log(qr/PANIC|failed to revert xid $xid, will retry/,
		$offset);
	my $log = slurp_file($node->logfile, $offset);
	unlike($log, qr/PANIC/, "$what: no PANIC");
	die "the server PANICked" if $log =~ /PANIC/;
	like(
		$log,
		qr/test_idxundo: error with resources at logical-revert-process-error/,
		"$what: the revert failed");
	like(
		$log,
		qr/failed to revert xid $xid, will retry/,
		"$what: the worker will retry");

	$node->safe_psql('postgres',
		"SELECT injection_points_detach('logical-revert-process-error')");
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$xid'"
		),
		"$what: entry is reverted on retry");
}

sub attach_error
{
	$node->safe_psql(
		'postgres', q{
SELECT injection_points_attach('logical-revert-process-error',
    'test_idxundo', 'idxundo_error_with_resources', ''::bytea);
});
}

# Control: the error, no checkpoint.
attach_error();
my $offset = -s $node->logfile;
my $xid = abort_into_atm(1);
check_retried($xid, $offset, 'without checkpoint');

# The same error for an entry whose UNDO is behind the redo pointer: hold
# the worker before it reverts the entry, checkpoint, then let it fail.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('logical-revert-before-process', 'wait')"
);
$offset = -s $node->logfile;
$xid = abort_into_atm(10001);
$node->wait_for_event('logical revert worker',
	'logical-revert-before-process');
my $batch_lsn = $node->safe_psql('postgres',
	"SELECT last_batch_lsn FROM idxundo_atm_entries() WHERE xid = '$xid'");
$node->safe_psql('postgres', 'CHECKPOINT');
cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT redo_lsn > '$batch_lsn' FROM pg_control_checkpoint()"),
	'eq', 't',
	'the redo pointer is past the entry\'s UNDO');
attach_error();
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('logical-revert-before-process');
SELECT injection_points_wakeup('logical-revert-before-process');
});
check_retried($xid, $offset, 'after checkpoint');

# The aborted rows are gone and the index agrees with the heap.
is( $node->safe_psql(
		'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE id BETWEEN 1 AND 20000;
}),
	'0',
	'no aborted row is visible through the index');
my ($rc, $stdout, $stderr) = $node->psql('postgres',
	"SELECT bt_index_check('t_pkey', heapallindexed => true)");
is($rc, 0, 'amcheck clean on t_pkey');
is($stderr, '', 'amcheck reported nothing on t_pkey');
unlike(
	slurp_file($node->logfile),
	qr/Assertion|WARNING:.*(?:buffer.*(?:pin|leak)|relcache reference leak)|TRAP:/,
	'no assertion or resource leak');

$node->stop;
done_testing();
