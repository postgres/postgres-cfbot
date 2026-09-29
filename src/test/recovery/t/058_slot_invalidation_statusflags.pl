# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A checkpoint that invalidates an obsolete replication slot must not corrupt
# the shared ProcGlobal->statusFlags array.
#
# The corruption is only noticed in builds with assertions enabled, by the
# assertion in ProcArrayEndTransaction() that compares a backend's flags with
# its entry in ProcGlobal->statusFlags[].
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1, extra => ['--wal-segsize=1']);

# No checkpoint may happen on its own: the single CHECKPOINT this test issues
# has to be the one that invalidates the slot, and it has to run while the
# VACUUM below is waiting for its lock.
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
min_wal_size = 2MB
max_wal_size = 1GB
max_slot_wal_keep_size = 1MB

# Cluster::init() turns this off, but a server that stays down would make
# teardown bail out before the failure is reported.  Let it come back instead,
# and report the crash from the log.
restart_after_crash = on
));
$node->start;

$node->safe_psql('postgres',
	'CREATE TABLE vactbl AS SELECT generate_series(1, 1000) AS i');
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('lagging', true)");

# The slot is not in use, so the checkpointer acquires it itself.  Leave it
# far enough behind that the next checkpoint has to invalidate it.
$node->advance_wal(2);

# The control session runs over a walsender connection on purpose: its PGPROC
# comes from the walsender free list, so it cannot take offset 0 in the proc
# array, which the VACUUM backend below has to own.
my $ctl = $node->background_psql('postgres', replication => 'database');

# vacuum_rel() sets PROC_IN_VACUUM before it opens the relation, so a
# conflicting lock makes VACUUM wait with the flag set.  At commit,
# ProcArrayEndTransaction() asserts that the flag in ProcGlobal->statusFlags[]
# still matches.
$ctl->query_safe('BEGIN');
$ctl->query_safe('LOCK TABLE vactbl IN SHARE UPDATE EXCLUSIVE MODE');

my $vac = $node->background_psql('postgres', on_error_stop => 0);
$vac->query_until(qr//, "VACUUM vactbl;\n");

# Wait through the control session.  poll_query_until() would connect another
# regular backend, which could take offset 0 in the proc array instead of the
# VACUUM backend.
my $blocked = '';
foreach my $i (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	$blocked = $ctl->query_safe(
		q{SELECT count(*) FROM pg_locks
		   WHERE relation = 'vactbl'::regclass AND NOT granted});
	last if $blocked eq '1';
	usleep(100_000);
}
is($blocked, '1', 'VACUUM is waiting for the table lock');
is( $ctl->query_safe(
		q{SELECT count(*) FROM pg_stat_activity
		   WHERE backend_type = 'client backend'}),
	'1',
	'the vacuuming backend is the only regular backend connected');

isnt(
	$ctl->query_safe(
		q{SELECT wal_status FROM pg_replication_slots
		   WHERE slot_name = 'lagging'}),
	'lost',
	'slot has not been invalidated yet');

my $log_offset = -s $node->logfile;

$ctl->query_safe('CHECKPOINT');

is( $ctl->query_safe(
		q{SELECT wal_status FROM pg_replication_slots
		   WHERE slot_name = 'lagging'}),
	'lost',
	'checkpoint invalidated the obsolete slot');

# Release the lock.  The VACUUM now runs to completion and commits, which is
# where the clobbered flags are noticed.
$ctl->query_safe('COMMIT');

# If the server crashed, this session is gone too, so the query may fail.
my $vacuumed = eval { $vac->query_safe('SELECT 1') };

ok(!$node->log_contains(qr/TRAP: failed Assert/, $log_offset),
	'no assertion failure while invalidating the slot');
ok(!$node->log_contains(qr/was terminated by signal/, $log_offset),
	'no backend was killed while invalidating the slot');
is($vacuumed, '1', 'the VACUUM committed and its session survived');

# If the server crashed, both sessions are already gone, so quit them inside
# eval.
eval { $vac->quit; };
eval { $ctl->quit; };

done_testing();
