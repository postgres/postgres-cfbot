# Copyright (c) 2026, PostgreSQL Global Development Group

# The always-running undo launcher's UndoGetWork() must not run a transaction
# while holding RollbackRequestLock, nor re-enter that lock to remove a request
# whose database is gone.  It did both: on a rollback request for a dropped
# database it called StartTransactionCommand()/dbid_exists()/CommitTransactionCommand()
# under the lock and then RollbackHTRemoveEntry(), which re-acquires the same
# LWLock exclusively -> self-deadlock.  The second acquire of an already-held
# LWLock blocks forever, inside HOLD_INTERRUPTS(), so the launcher is wedged.
#
# idxundo_plant_dropped_db_request() plants such a request; within a cycle the
# launcher processes it.  A wedged launcher never absorbs a ProcSignalBarrier,
# so DROP DATABASE -- which waits for every process to absorb one (see
# 071_perbackend_undo.pl) -- hangs.  With the fix the launcher checks the
# database with the lock released and drops the stale request, so DROP DATABASE
# completes.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('idxundo_launcher_dropped_db');
$node->init;
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_idxundo; CREATE DATABASE barrier_probe');

# Plant a rollback request whose database (InvalidOid) does not exist and let
# the launcher reach it (its cycle is 100ms).
$node->safe_psql('postgres', 'SELECT idxundo_plant_dropped_db_request()');
sleep(1);

# DROP DATABASE needs the launcher to absorb its ProcSignalBarrier; a
# self-deadlocked launcher never does, so this hangs on the buggy code.
my $timed_out;
$node->psql(
	'postgres',
	'DROP DATABASE barrier_probe',
	timeout => 30,
	timed_out => \$timed_out);
ok(!$timed_out,
	'the undo launcher stays responsive after a dropped-database request');

# A self-deadlocked launcher holds RollbackRequestLock forever; stop hard.
$node->stop($timed_out ? 'immediate' : 'fast');
done_testing();
