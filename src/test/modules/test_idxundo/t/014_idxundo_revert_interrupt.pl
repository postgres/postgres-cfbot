# Copyright (c) 2026, PostgreSQL Global Development Group

# A logical revert worker in the middle of a long UNDO chain must answer
# pg_terminate_backend(), pg_cancel_backend() and a fast shutdown, and the
# entry it did not finish must stay in the ATM and be reverted later.
#
# The injection point undo-apply-before-batch is hit before each batch of
# the chain; test_idxundo's idxundo_sleep makes each batch take seconds, so
# the rest of the chain takes several times longer than the bound the
# request must be answered in.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $sleep_ms = 2000;    # per batch: the chain takes minutes
my $bound = 30;         # seconds a request may take to be answered

my $node = PostgreSQL::Test::Cluster->new('idxundo_revert_interrupt');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
max_logical_revert_workers = 2
logical_revert_naptime = 100ms
# many batches: a chain of 5000 index inserts has 50
undo_batch_record_limit = 100
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_idxundo;
CREATE EXTENSION amcheck;
CREATE TABLE t (id int primary key)
  WITH (index_undo = on, autovacuum_enabled = off);
});

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

sub atm_has
{
	my ($xid) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM idxundo_atm_entries() WHERE xid = '$xid'") eq
	  '1';
}

# Start reverting a large abort slowly; returns its xid and the worker's
# pid once the worker is in the middle of the chain.
sub worker_mid_chain
{
	my ($from) = @_;
	$node->safe_psql(
		'postgres', qq{
SELECT injection_points_attach('undo-apply-before-batch', 'test_idxundo',
  'idxundo_sleep', '$sleep_ms'::bytea);
});
	my $xid = abort_into_atm($from);
	$node->wait_for_event('logical revert worker', 'PgSleep');
	my $pid = $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'logical revert worker'"
	);
	ok(atm_has($xid), "$from: the worker is reverting the entry");
	return ($xid, $pid);
}

# Seconds until $cond returns true, or undef if it does not within $limit
# seconds.
sub answered_within
{
	my ($cond, $limit) = @_;
	my $start = time;
	while (time - $start < $limit)
	{
		return time - $start if $cond->();
		usleep(100_000);
	}
	return undef;
}

# The entry is reverted and forgotten, and the index agrees with the heap.
sub check_reverted
{
	my ($xid, $what) = @_;
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) = 0 FROM idxundo_atm_entries() WHERE xid = '$xid'"
		),
		"$what: the entry is reverted");
	is( $node->safe_psql(
			'postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE id > 0;
}),
		'0',
		"$what: no aborted row is visible through the index");
	my ($rc, $stdout, $stderr) = $node->psql('postgres',
		"SELECT bt_index_check('t_pkey', heapallindexed => true)");
	is("$rc:$stderr", '0:', "$what: amcheck clean on t_pkey");
}

# Get a revert worker running for this database: the launcher starts one at
# startup for a database with ATM entries, and the worker then stays.
my $xid = abort_into_atm(1000001);
$node->restart;
check_reverted($xid, 'warm-up');

###########################################################################
# pg_terminate_backend() of the worker in the middle of the chain.
###########################################################################
my $pid;
($xid, $pid) = worker_mid_chain(1);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
my $took = answered_within(
	sub {
		$node->safe_psql('postgres',
			"SELECT count(*) FROM pg_stat_activity WHERE pid = $pid") eq '0';
	},
	2 * $bound);
ok( defined $took && $took < $bound,
	defined $took
	? sprintf("pg_terminate_backend answered in %.1f s", $took)
	: "pg_terminate_backend not answered in " . 2 * $bound . " s");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('undo-apply-before-batch')");

# The postmaster starts the worker again only after bgw_restart_time; at
# startup the launcher starts one at once.
$node->restart;
check_reverted($xid, 'terminate');

###########################################################################
# pg_cancel_backend() of the worker in the middle of the chain: the walk
# fails like any other error, and the same worker retries the entry.
###########################################################################
my $log_offset = -s $node->logfile;
($xid, $pid) = worker_mid_chain(20001);
$node->safe_psql('postgres', "SELECT pg_cancel_backend($pid)");
$took = answered_within(
	sub {
		$node->log_contains(
			qr/\[$pid\] ERROR:  canceling statement due to user request/,
			$log_offset);
	},
	2 * $bound);
ok( defined $took && $took < $bound,
	defined $took
	? sprintf("pg_cancel_backend answered in %.1f s", $took)
	: "pg_cancel_backend not answered in " . 2 * $bound . " s");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('undo-apply-before-batch')");
check_reverted($xid, 'cancel');
ok( $node->log_contains(
		qr/failed to revert xid $xid, will retry/, $log_offset),
	'cancel: the worker retried the entry');
is( $node->safe_psql(
		'postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'logical revert worker'"
	),
	$pid,
	'cancel: the same worker is still running');

###########################################################################
# A fast shutdown while the worker is in the middle of the chain.
###########################################################################
($xid, $pid) = worker_mid_chain(10001);
my $start = time;
my $stopped = $node->stop('fast', fail_ok => 1, timeout => $bound);
$took = time - $start;
ok($stopped, 'fast shutdown completed while the worker reverts');
cmp_ok($took, '<', $bound,
	sprintf("fast shutdown took %.1f s, under $bound s", $took));
$node->stop('immediate') unless $stopped;

# Without a revert worker, the entry is still in the ATM.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
$node->start;
ok(atm_has($xid), 'shutdown: the entry is outstanding after the restart');

$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
check_reverted($xid, 'shutdown');

unlike(
	slurp_file($node->logfile),
	qr/Assertion|PANIC|WARNING:.*(?:buffer.*(?:pin|leak)|relcache reference leak)|TRAP:/,
	'no assertion, PANIC or resource leak');

$node->stop;
done_testing();
