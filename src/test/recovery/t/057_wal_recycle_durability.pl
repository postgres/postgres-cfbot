# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Test the write-path durability barrier for batched WAL-segment recycling.
#
# When the checkpointer recycles old WAL segments into future ones it
# renames them and defers making the renames durable to a single fsync of
# pg_wal at the end of the pass (plus a per-file fsync of each recycled
# segment).  A recycled segment becomes usable by the WAL write path as
# soon as it is renamed, so if the write frontier reaches such a segment
# before the checkpointer's batched fsync, XLogFileInit() must make the
# rename durable itself, the write-path "durability barrier"
# (EnsureXLogSegDirDurable()).
#
# This test also makes a foreground WAL segment installation leap past the
# pending recycled segments.  That advances the durability frontier beyond
# them, so the active-batch marker must override the frontier and force the
# write-path barrier.  It then crashes the server with the batch still pending
# and checks that committed data survives crash recovery.
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
max_wal_size = 1GB
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

# Generate old segments for the checkpoint to recycle, but do not checkpoint
# yet.  This keeps the known-durable frontier at the current segment.
for (1 .. 8)
{
	$node->safe_psql('postgres',
		q{INSERT INTO filler SELECT g, repeat('x', 900) FROM generate_series(1, 20000) g}
	);
	$node->safe_psql('postgres', q{SELECT pg_switch_wal()});
}

# Start a checkpoint in the background.  First pause immediately before old
# WAL removal so a writer can prepare a temporary segment for installation.
# The second pause keeps the resulting recycle batch pending.
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_safe(
	q{
	select injection_points_attach('checkpoint-before-old-wal-removal', 'wait');
	select injection_points_attach('wal-recycle-before-batch-fsync', 'wait');
});
$checkpoint->query_until(
	qr/starting_checkpoint/, q(\echo starting_checkpoint
checkpoint;
\q
));

# The checkpoint has fixed its WAL-removal horizon but has not scanned pg_wal.
$node->wait_for_event('checkpointer', 'checkpoint-before-old-wal-removal');

# Make the next segment slot empty.  The writer will prepare that segment while
# the checkpoint is paused, and the checkpoint will recycle an old segment into
# the slot before the writer can install its temporary file.
my $target_wal = $node->safe_psql(
	'postgres', q{
	SELECT pg_walfile_name(pg_current_wal_insert_lsn() +
		pg_size_bytes(current_setting('wal_segment_size')))
});
my $target_path = $node->data_dir . "/pg_wal/$target_wal";
unlink($target_path) if -e $target_path;
ok(!-e $target_path, 'next WAL segment slot is empty');

# Prepare the writer's two cached wait callbacks outside its WAL critical
# section: one before installing its temporary segment and one in the
# write-path durability barrier.
my $writer = $node->background_psql('postgres');
$writer->query_safe(
	q{
	select injection_points_attach('wal-recycle-before-segment-install', 'wait');
	select injection_points_attach('wal-recycle-after-segment-install', 'wait');
	select injection_points_attach('wal-recycle-before-write-path-fsync', 'wait');
});

for my $point (
	'wal-recycle-before-segment-install',
	'wal-recycle-after-segment-install',
	'wal-recycle-before-write-path-fsync')
{
	$writer->query_until(
		qr/priming_writer/,
		"\\echo priming_writer\nselect injection_points_run('$point');\n"
		  . "\\echo writer_primed\n");
	$node->wait_for_event('client backend', $point);
	$node->safe_psql('postgres',
		"select injection_points_wakeup('$point')");
	$writer->query_until(qr/writer_primed/, '');
}

$writer->query_safe(
	q{
	select injection_points_load('wal-recycle-before-segment-install');
	select injection_points_load('wal-recycle-after-segment-install');
	select injection_points_load('wal-recycle-before-write-path-fsync');
});

# Switch into the missing target.  The writer creates and fsyncs a temporary
# segment, then pauses immediately before attempting to install it.
$writer->query_until(
	qr/starting_writer/, q(\echo starting_writer
SELECT pg_switch_wal();
INSERT INTO t VALUES (1, 'committed-in-window');
\echo writer_finished
));
$node->wait_for_event('client backend',
	'wal-recycle-before-segment-install');

# Let old-WAL cleanup fill the missing target and pause before its batch fsync.
$node->safe_psql(
	'postgres', q{
	select injection_points_detach('checkpoint-before-old-wal-removal');
	select injection_points_wakeup('checkpoint-before-old-wal-removal');
});
$node->wait_for_event('checkpointer', 'wal-recycle-before-batch-fsync');
ok(-e $target_path, 'checkpoint recycled a segment into the writer target');

# The writer now finds its target occupied, skips the batch's pending names,
# and durably installs its temporary file at a higher segment.  That advances
# the high watermark beyond the target while the recycle batch remains active.
$node->safe_psql(
	'postgres', q{
	select injection_points_detach('wal-recycle-before-segment-install');
	select injection_points_wakeup('wal-recycle-before-segment-install');
});
$node->wait_for_event('client backend',
	'wal-recycle-after-segment-install');
pass('foreground WAL installation advanced the durability frontier');
$node->safe_psql(
	'postgres', q{
	select injection_points_detach('wal-recycle-after-segment-install');
	select injection_points_wakeup('wal-recycle-after-segment-install');
});

# Despite that higher watermark, the active batch must force the durability
# barrier before the writer can use the pending recycled target.
$node->wait_for_event('client backend',
	'wal-recycle-before-write-path-fsync');
pass('active recycle batch overrode the advanced durability frontier');

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
	'2', 'row committed before crash');

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
	'baseline,committed-in-window',
	'committed row survived crash with the recycle-durability window open');

done_testing();
