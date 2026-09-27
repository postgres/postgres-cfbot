
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# UNDO, the sLog and FLUX allocate no large fixed shared memory.
#
# A cluster that never uses FLUX or UNDO should cost about what master does.
# The transaction sLog (the ATM) reserved a 512 kB DSA region at startup and
# FLUX an 8 MB dirty-page map.  The ATM now lives in a DSA created when it is
# first written, and the dirty-page map is gone.  The 1 MB bound on what is
# left is a coarse guard, not the goal: see the commit message.  The ATM must still work
# from there: an instant abort, the revert worker, a checkpoint and a crash
# restart.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
log_min_messages = debug1
undo_instant_abort_threshold = 0
logical_revert_naptime = 100
));
$node->start;

my $fixed =
  q{SELECT coalesce(sum(allocated_size), 0) FROM pg_shmem_allocations
  WHERE name ~* 'undo|flux|slog|secondary log|aborted|revert'};
my $created = qr/created the aborted transaction map/;

is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_shmem_allocations WHERE name = 'FLUX DirtyMap'}
	),
	'0',
	'no fixed FLUX dirty map');
cmp_ok(
	$node->safe_psql(
		'postgres',
		q{SELECT coalesce(sum(allocated_size), 0) FROM pg_shmem_allocations
		  WHERE name = 'Secondary Log State'}),
	'<',
	1024,
	'transaction sLog reserves only its lock and handles');
cmp_ok(
	$node->safe_psql('postgres', $fixed),
	'<',
	1024 * 1024,
	'UNDO, sLog and FLUX fixed shared memory below 1 MB');

# A checkpoint reads the ATM; that must not create it.  Its DSA area is
# created by the first transaction that writes UNDO.
$node->safe_psql('postgres', 'CHECKPOINT');
ok(!$node->log_contains($created), 'no ATM before use');

# An instant abort records the transaction in the ATM, creating its DSA, and
# the revert worker undoes it.
$node->safe_psql(
	'postgres', q{
CREATE TABLE t (id int PRIMARY KEY, v int) USING heap
  WITH (index_undo = on, autovacuum_enabled = off);
INSERT INTO t VALUES (0, 0);
});
$node->safe_psql('postgres',
	'BEGIN; INSERT INTO t SELECT g, g FROM generate_series(1, 100) g; ROLLBACK;'
);
ok($node->log_contains($created), 'ATM created on use');
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_stat_get_undo_retention()
		  WHERE holder = 'aborted'}),
	'revert worker drained the ATM');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1', 'aborted rows invisible');

# A deferred abort outlives a checkpoint and a crash.
$node->safe_psql('postgres',
	'BEGIN; INSERT INTO t SELECT g, g FROM generate_series(101, 200) g; ROLLBACK;'
);
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1', 'aborted rows stay invisible after a crash');
$node->safe_psql('postgres', 'VACUUM t');
is( $node->safe_psql(
		'postgres',
		'SET enable_seqscan = off; SELECT count(*) FROM t WHERE id >= 0'),
	'1',
	'index agrees after a crash');

$node->stop;
done_testing();
