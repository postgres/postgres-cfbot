# Per-backend UNDO engine lifecycle and crash-recovery smoke test.
# The engine's background workers always run; this commit adds no table AM
# that produces per-backend undo, so here they only initialize and idle.  This
# test confirms the engine initializes, both workers launch, and a
# crash/recover cycle is clean.  Reclamation of undo written by a table AM is
# tested with the AMs that produce it.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_primary');
$node->init;
$node->start;

# 1. server up
my $up = $node->safe_psql('postgres', 'SELECT 1');
is($up, '1', 'server up');

# 2. txn + rollback + commit + checkpoint are correct (heap; engine dormant)
$node->safe_psql('postgres', 'CREATE TABLE t(a int)');
$node->safe_psql('postgres', 'BEGIN; INSERT INTO t VALUES (1),(2); ROLLBACK');
$node->safe_psql('postgres', 'INSERT INTO t VALUES (3)');
$node->safe_psql('postgres', 'CHECKPOINT');
my $cnt = $node->safe_psql('postgres', 'SELECT count(*) FROM t');
is($cnt, '1', 'rollback discarded uncommitted rows, commit survived');

# 3. bgworkers actually present
my $log = slurp_file($node->logfile);
like($log, qr/UNDO worker started/, 'undo worker launched');
like($log, qr/discard worker started/, 'discard worker launched');

# 4. crash + recover cleanly
$node->stop('immediate');
$node->start;
my $after = $node->safe_psql('postgres', 'SELECT count(*) FROM t');
is($after, '1', 'data intact after crash recovery');
my $rlog = slurp_file($node->logfile);
unlike(
	$rlog,
	qr/PANIC|inconsistent|could not|corrupt/i,
	'clean crash recovery, no PANIC/corruption');

# 5. The engine's workers are always running, so each must absorb
# ProcSignalBarriers: one that never does stalls every
# WaitForProcSignalBarrier() caller.  DROP DATABASE emits a barrier and waits
# for every process to absorb it, so it is a direct probe.
$node->safe_psql('postgres', 'CREATE DATABASE barrier_probe');
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	'DROP DATABASE barrier_probe',
	timeout => $PostgreSQL::Test::Utils::timeout_default);
is($ret, 0,
	'DROP DATABASE completes: undo workers absorb ProcSignalBarriers');

$node->stop;
done_testing();
