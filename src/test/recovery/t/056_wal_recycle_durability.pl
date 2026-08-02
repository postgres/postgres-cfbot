# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Test the write-path durability barrier for batched WAL-segment recycling.
#
# When the checkpointer recycles old WAL segments into future ones it renames
# them and defers making the renames durable to a single fsync of pg_wal at the
# end of the pass (plus a per-file fsync of each recycled segment).  A recycled
# segment becomes usable by the WAL write path as soon as it is renamed, so if
# the write frontier reaches such a segment before the checkpointer's batched
# fsync, XLogFileInit() must make the rename durable itself, the write-path
# "durability barrier" (EnsureXLogSegDirDurable()).
#
# This test opens that window with an injection point placed just after the
# recycle renames but before the batched fsync, then verifies that a WAL writer
# trying to use a just-recycled segment reaches the write-path barrier.  It
# also crashes the server with the window still open and checks that committed
# data survives crash recovery.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_recycle = on
min_wal_size = 32MB
max_wal_size = 64MB
checkpoint_timeout = 1h
log_checkpoints = on
});
$node->start;

# Skip if the injection_points extension is not installed, e.g. under
# installcheck where the module may not be present.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t (id int primary key, v text);
	INSERT INTO t VALUES (0, 'baseline');
	CREATE TABLE filler (id int, pad text);
});

# Build up a pool of recycled segments and advance the durability frontier by
# generating several segments' worth of WAL and checkpointing.
for (1 .. 8)
{
	$node->safe_psql('postgres',
		q{INSERT INTO filler SELECT g, repeat('x', 900) FROM generate_series(1, 20000) g}
	);
	$node->safe_psql('postgres', q{SELECT pg_switch_wal()});
}
$node->safe_psql('postgres', q{CHECKPOINT});

# Generate more WAL so the next checkpoint has fresh future slots to recycle.
$node->safe_psql('postgres',
	q{INSERT INTO filler SELECT g, repeat('y', 900) FROM generate_series(1, 60000) g}
);
$node->safe_psql('postgres', q{SELECT pg_switch_wal()});

# Start a checkpoint in the background and make it pause right after the recycle
# renames but before the batched pg_wal fsync.
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_safe(
	q{select injection_points_attach('wal-recycle-before-batch-fsync', 'wait')});
$checkpoint->query_until(
	qr/starting_checkpoint/, q(\echo starting_checkpoint
checkpoint;
\q
));

# Wait until the checkpointer is parked in the batch window: the recycle renames
# are done but not yet durable.
$node->wait_for_event('checkpointer', 'wal-recycle-before-batch-fsync');

# The window is open.  Drive the WAL write frontier into the just-recycled
# segments in a background session.  Preload the injection-point callback in
# that backend because the barrier runs inside a critical section.
my $writer = $node->background_psql('postgres');
$writer->query_safe(
	q{select injection_points_attach('wal-recycle-before-write-path-fsync', 'wait')}
);

# Run the wait callback once outside the WAL critical section to initialize
# its shared wait-event state in this backend.
$writer->query_until(
	qr/priming_writer/,
	q(\echo priming_writer
select injection_points_run('wal-recycle-before-write-path-fsync');
\echo writer_primed
));
$node->wait_for_event('client backend',
	'wal-recycle-before-write-path-fsync');
$node->safe_psql('postgres',
	q{select injection_points_wakeup('wal-recycle-before-write-path-fsync')});
$writer->query_until(qr/writer_primed/, '');

$writer->query_safe(
	q{select injection_points_load('wal-recycle-before-write-path-fsync')});

my $writer_sql = "\\echo starting_writer\n";
for my $j (1 .. 6)
{
	$writer_sql .= "SELECT pg_switch_wal();\n";
	$writer_sql .=
	  "INSERT INTO t VALUES ($j, 'committed-in-window-$j');\n";
}
$writer_sql .= "\\echo writer_finished\n";
$writer->query_until(qr/starting_writer/, $writer_sql);

# The first recycled segment must not be handed to the writer until its rename
# has been made durable.  Reaching this point proves that the slow path in
# EnsureXLogSegDirDurable() is actually exercised.
$node->wait_for_event('client backend',
	'wal-recycle-before-write-path-fsync');
pass('WAL writer reached the recycle-durability barrier');

# Detach first so later segment switches do not wait again, then let the first
# call make the recycled segment and pg_wal durable.
$node->safe_psql(
	'postgres', q{
	select injection_points_detach('wal-recycle-before-write-path-fsync');
	select injection_points_wakeup('wal-recycle-before-write-path-fsync');
});
$writer->query_until(qr/writer_finished/, '');
$writer->quit;

is( $node->safe_psql('postgres', q{SELECT count(*) FROM t}),
	'7', 'all rows committed before crash');

# Crash with the window still open: the checkpointer never ran its batched
# fsync, so durability of the recycled segments rests entirely on the barrier.
$node->stop('immediate');

# The checkpoint session's connection died with the crash; reap it quietly.
eval { $checkpoint->quit; };

# Crash recovery.
$node->start;

# Every committed row must still be present.
is( $node->safe_psql(
		'postgres', q{SELECT string_agg(v, ',' ORDER BY id) FROM t}),
	'baseline,committed-in-window-1,committed-in-window-2,committed-in-window-3,'
	  . 'committed-in-window-4,committed-in-window-5,committed-in-window-6',
	'committed rows survived crash with the recycle-durability window open');

done_testing();
