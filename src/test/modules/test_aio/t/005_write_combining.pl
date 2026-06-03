# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('write-combining');
$node->init();

$node->append_conf(
	'postgresql.conf', qq(
max_wal_size = '4GB'
checkpoint_timeout = '1d'
autovacuum = off
io_combine_limit = '128kB'
bgwriter_lru_maxpages = 0
));

$node->start();

$node->safe_psql('postgres', 'CREATE EXTENSION test_aio');

my $block_size = $node->safe_psql('postgres',
	"SELECT current_setting('block_size')::int");

test_checkpointer_combines_writes($node, $block_size);
test_regular_backend_combines_writes($node, $block_size);
test_eager_clean_combines_writes($node, $block_size);
test_copy_from_combines_writes($node, $block_size);
test_vacuum_combines_writes($node, $block_size);
test_bgwriter_combines_writes($node, $block_size);

$node->stop();

done_testing();

sub io_stat_writes
{
	my ($node, $backend_type, $context) = @_;

	my $result = $node->safe_psql(
		'postgres', qq(
	SELECT COALESCE(sum(writes), 0)::bigint,
		COALESCE(sum(write_bytes), 0)::bigint,
		COALESCE((sum(write_bytes) / NULLIF(sum(writes), 0))::bigint, 0)
	FROM pg_stat_io
	WHERE backend_type = '$backend_type'
	AND object = 'relation'
	AND context = '$context';
	));

	return split /\|/, $result;
}

sub io_stat_evictions
{
	my ($node, $backend_type, $context) = @_;

	return $node->safe_psql(
		'postgres', qq(
	SELECT COALESCE(sum(evictions), 0)::bigint
	FROM pg_stat_io
	WHERE backend_type = '$backend_type'
	AND object = 'relation'
	AND context = '$context';
	));
}

sub assert_combined_writes
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $label, $backend_type, $context, $block_size) = @_;
	my ($writes, $write_bytes, $avg_write_bytes) =
	  io_stat_writes($node, $backend_type, $context);

	note "$label: writes=$writes write_bytes=$write_bytes avg_write_bytes=$avg_write_bytes";
	ok($writes > 0, "$label wrote buffers");
	ok($avg_write_bytes > $block_size, "$label combined writes");
}


sub assert_writes
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $label, $backend_type, $context, $expected_writes, $expected_bytes) = @_;
	my ($writes, $write_bytes, $avg_write_bytes) =
	  io_stat_writes($node, $backend_type, $context);

	note "$label: writes=$writes write_bytes=$write_bytes avg_write_bytes=$avg_write_bytes";

	is($writes, $expected_writes, "$label write count");
	is($write_bytes, $expected_bytes, "$label write bytes");
}

sub assert_writes_at_least
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $label, $backend_type, $context, $expected_writes, $expected_bytes) = @_;
	my ($writes, $write_bytes, $avg_write_bytes) =
	  io_stat_writes($node, $backend_type, $context);

	note "$label: writes=$writes write_bytes=$write_bytes avg_write_bytes=$avg_write_bytes";
	ok($writes >= $expected_writes,
		"$label wrote at least $expected_writes times");
	ok($write_bytes >= $expected_bytes,
		"$label wrote at least $expected_bytes bytes");
}

sub assert_evictions_at_least
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $label, $expected_evictions) = @_;
	my $evictions = io_stat_evictions($node, 'client backend', 'normal');

	note "$label: evictions=$evictions";
	ok($evictions >= $expected_evictions,
		"$label evicted at least $expected_evictions buffers");
}

sub assert_blocks_dirty
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $table, $blocks, $expected, $label) = @_;

	is($node->safe_psql('postgres',
		"SELECT true = ALL (rel_blocks_are_dirty('$table', ARRAY[$blocks]))"),
		$expected, $label);
}

sub assert_any_blocks_dirty
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $table, $blocks, $expected, $label) = @_;

	is($node->safe_psql('postgres',
		"SELECT true = ANY (rel_blocks_are_dirty('$table', ARRAY[$blocks]))"),
		$expected, $label);
}

sub flush_and_reset_io_stats
{
	my ($node, $psql) = @_;

	$psql->query_safe('SELECT pg_stat_force_next_flush()');
	$node->safe_psql('postgres', "SELECT pg_stat_reset_shared('io')");
}

sub dirty_blocks
{
	my ($psql, $table, $blocks) = @_;

	$psql->query_safe(
		"SELECT make_blocks_unused_dirty_flushed('$table', ARRAY[$blocks])");
}

sub run_bgwriter_cleaner
{
	my $psql = shift;

	$psql->query_safe('SELECT run_bgwriter_cleaner(1000)');
	$psql->query_safe('SELECT pg_stat_force_next_flush()');
}

sub allocate_until_blocks_clean
{
	my ($node, $psql, $table, $blocks, $filler, $label) = @_;
	my $shared_buffers_blocks = $node->safe_psql(
		'postgres',
		"SELECT pg_size_bytes(current_setting('shared_buffers')) / current_setting('block_size')::int"
	);
	my $extend_by = 1024;
	my $allocated = 0;
	my $max_allocations = 3 * $shared_buffers_blocks;

	while ($node->safe_psql('postgres',
			"SELECT true = ANY (rel_blocks_are_dirty('$table', ARRAY[$blocks]))") eq 't')
	{
		if ($allocated >= $max_allocations)
		{
			die "$label: blocks $blocks still dirty after $allocated buffer allocations";
		}

		$psql->query_safe("SELECT grow_rel('$filler'::regclass, $extend_by)");
		$allocated += $extend_by;
	}

	note "$label: allocated $allocated buffers to reach regular backend victims";
	$psql->query_safe('SELECT pg_stat_force_next_flush()');
}

sub test_copy_from_combines_writes
{
	my ($node, $block_size) = @_;

	$node->safe_psql(
		'postgres', qq(
	CREATE UNLOGGED TABLE wc_copy (id int, payload text);
	CHECKPOINT;
	));

	# Feed the rows to "COPY ... FROM STDIN" inline on psql's stdin.
	my $rows = 200000;
	my $payload = '0' x 200;
	my $copy_data = '';
	$copy_data .= "$_\t$payload\n" for (1 .. $rows);

	$node->safe_psql('postgres', "SELECT pg_stat_reset_shared('io')");
	$node->safe_psql('postgres',
		"COPY wc_copy FROM STDIN;\n" . $copy_data . "\\.\n");
	$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');

	assert_combined_writes($node, 'copy from', 'client backend', 'bulkwrite',
		$block_size);
	is($node->safe_psql('postgres', "SELECT count(*) FROM wc_copy"),
		$rows, 'copy from inserted rows');
}

sub test_checkpointer_combines_writes
{
	my ($node, $block_size) = @_;
	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	$node->safe_psql(
		'postgres', qq(
	CREATE TABLE wc_checkpointer (id int, payload text);
	INSERT INTO wc_checkpointer SELECT g, repeat('y', 200) FROM generate_series(1, 1000) AS g;
	SELECT flush_rel_buffers('wc_checkpointer'::regclass);
	CHECKPOINT;
	));

	####
	# Test one big combined write by checkpointer.
	####

	dirty_blocks($psql, 'wc_checkpointer', '0,1,2,3,4,5');
	assert_blocks_dirty($node, 'wc_checkpointer', '0,1,2,3,4,5', 't',
		'contiguous buffers are dirty before checkpoint');

	flush_and_reset_io_stats($node, $psql);
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');

	assert_combined_writes($node, 'contiguous checkpointer', 'checkpointer',
		'normal', $block_size);
	assert_any_blocks_dirty($node, 'wc_checkpointer', '0,1,2,3,4,5', 'f',
		'checkpointer wrote contiguous dirty buffers');

	####
	# Test multiple single block writes when interspersed blocks are not in
	# shared buffers.
	####

	$psql->query_safe(
		"SELECT invalidate_rel_blocks('wc_checkpointer', ARRAY[1,3,5])");
	dirty_blocks($psql, 'wc_checkpointer', '0,2,4');
	flush_and_reset_io_stats($node, $psql);
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');

	assert_writes_at_least($node, 'nonresident gaps checkpointer', 'checkpointer',
		'normal', 3, 3 * $block_size);
	assert_any_blocks_dirty($node, 'wc_checkpointer', '0,2,4', 'f',
		'checkpointer wrote dirty buffers separated by nonresident gaps');

	$psql->quit();
}

# A higher level test that might be too expensive to commit
sub test_regular_backend_combines_writes
{
	my ($node, $block_size) = @_;
	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# Keep unrelated dirty buffers out of the client-backend write statistics.
	$node->safe_psql('postgres', 'CHECKPOINT');

	$node->safe_psql(
		'postgres', qq(
	CREATE TABLE wc_backend (id int, payload text);
	INSERT INTO wc_backend SELECT g, repeat('y', 200) FROM generate_series(1, 1000) AS g;
	SELECT flush_rel_buffers('wc_backend'::regclass);
	CREATE UNLOGGED TABLE wc_backend_filler (id int);
	CHECKPOINT;
	));

	####
	# Test one big combined write from regular backend buffer allocation.
	####

	dirty_blocks($psql, 'wc_backend', '0,1,2,3,4,5');
	assert_blocks_dirty($node, 'wc_backend', '0,1,2,3,4,5', 't',
		'contiguous buffers are dirty before regular backend allocation');

	flush_and_reset_io_stats($node, $psql);
	allocate_until_blocks_clean($node, $psql, 'wc_backend', '0,1,2,3,4,5',
		'wc_backend_filler', 'contiguous regular backend');

	# Assert that combining happened at all, which shouldn't fail even if the
	# more specific test below it flakes.
	assert_combined_writes($node, 'contiguous regular backend', 'client backend',
		'normal', $block_size);
	# Unlike the checkpointer/bgwriter/eager-clean cases, we force writes here
	# by growing a filler relation until the clock sweep evicts our blocks. The
	# same sweep may also evict unrelated dirty pages in the 'normal' context
	# (e.g. a catalog page freshly dirtied by a hint bit), so this exact write
	# count test may fail. That's okay since we think this whole test routine is
	# probably too expensive to commit.
	assert_writes($node, 'contiguous regular backend', 'client backend',
		'normal', 1, 6 * $block_size);
	assert_evictions_at_least($node, 'contiguous regular backend', 6);
	assert_any_blocks_dirty($node, 'wc_backend', '0,1,2,3,4,5', 'f',
		'regular backend wrote contiguous dirty buffers');

	####
	# Test multiple single block writes when interspersed blocks are not in
	# shared buffers.
	####

	$psql->query_safe(
		"SELECT invalidate_rel_blocks('wc_backend', ARRAY[1,3,5])");
	dirty_blocks($psql, 'wc_backend', '0,2,4');
	flush_and_reset_io_stats($node, $psql);

	allocate_until_blocks_clean($node, $psql, 'wc_backend', '0,2,4',
		'wc_backend_filler', 'nonresident gaps regular backend');
	# The three dirty blocks are separated by non-resident gaps, so they must
	# not be combined: at least three separate writes. As above, stray dirty
	# pages evicted by the same sweep may push the count higher, so assert "at
	# least" rather than an exact count.
	assert_writes_at_least($node, 'nonresident gaps regular backend', 'client backend',
		'normal', 3,
		3 * $block_size);
	assert_evictions_at_least($node, 'nonresident gaps regular backend', 3);
	assert_any_blocks_dirty($node, 'wc_backend', '0,2,4', 'f',
		'regular backend wrote dirty buffers separated by nonresident gaps');

	$psql->quit();
}

sub test_eager_clean_combines_writes
{
	my ($node, $block_size) = @_;
	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	$node->safe_psql(
		'postgres', qq(
	CREATE TABLE wc_victim (id int, payload text);
	INSERT INTO wc_victim SELECT g, repeat('y', 200) FROM generate_series(1, 1000) AS g;
	SELECT flush_rel_buffers('wc_victim'::regclass);
	CHECKPOINT;
	));

	####
	# Test one big combined write when WriteBufferAndNeighbors() is called directly.
	####

	dirty_blocks($psql, 'wc_victim', '0,1,2,3,4,5');
	assert_blocks_dirty($node, 'wc_victim', '0,1,2,3,4,5', 't',
		'contiguous buffers are dirty before direct eager clean');

	flush_and_reset_io_stats($node, $psql);
	$psql->query_safe("SELECT eager_clean_rel_block('wc_victim', 0)");
	$psql->query_safe('SELECT pg_stat_force_next_flush()');

	assert_writes($node, 'contiguous direct eager clean', 'client backend',
		'normal', 1, 6 * $block_size);
	assert_any_blocks_dirty($node, 'wc_victim', '0,1,2,3,4,5', 'f',
		'direct eager clean wrote contiguous dirty buffers');

	####
	# Test multiple single block writes when interspersed blocks are not in
	# shared buffers.
	####

	$psql->query_safe(
		"SELECT invalidate_rel_blocks('wc_victim', ARRAY[1,3,5])");
	dirty_blocks($psql, 'wc_victim', '0,2,4');
	flush_and_reset_io_stats($node, $psql);

	$psql->query_safe("SELECT eager_clean_rel_block('wc_victim', 0)");
	$psql->query_safe("SELECT eager_clean_rel_block('wc_victim', 2)");
	$psql->query_safe("SELECT eager_clean_rel_block('wc_victim', 4)");
	$psql->query_safe('SELECT pg_stat_force_next_flush()');
	assert_writes($node, 'nonresident gaps direct eager clean', 'client backend',
		'normal', 3,
		3 * $block_size);
	assert_any_blocks_dirty($node, 'wc_victim', '0,2,4', 'f',
		'direct eager clean wrote dirty buffers separated by nonresident gaps');

	$psql->quit();
}

sub test_vacuum_combines_writes
{
	my ($node, $block_size) = @_;

	# ~120 blocks: enough to wrap vacuum's ring (sized below) several
	# times, with dead tuples on every page so pruning dirties every page
	# vacuum reads into the ring.
	#
	# This uses a regular (logged) table, so at least one test covers logged
	# tables. We don't expect WAL flush timing to affect the results because the
	# target buffer is always included in the batch and will do a combined write
	# regardless of whether or not it has to flush WAL.
	$node->safe_psql(
		'postgres', qq(
	CREATE TABLE wc_vacuum (id int, payload text);
	INSERT INTO wc_vacuum SELECT g, repeat('y', 200) FROM generate_series(1, 3600) AS g;
	DELETE FROM wc_vacuum WHERE id % 5 = 0;
	SELECT flush_rel_buffers('wc_vacuum'::regclass);
	SELECT evict_rel('wc_vacuum'::regclass);
	));

	$node->safe_psql('postgres', "SELECT pg_stat_reset_shared('io')");
	$node->safe_psql('postgres',
		"VACUUM (BUFFER_USAGE_LIMIT '256kB') wc_vacuum");
	$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');

	assert_combined_writes($node, 'vacuum', 'client backend', 'vacuum',
		$block_size);
}

sub test_bgwriter_combines_writes
{
	my ($node, $block_size) = @_;
	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# Keep unrelated dirty buffers out of the client-backend write statistics.
	$node->safe_psql('postgres', 'CHECKPOINT');

	# We create a relation and then flush all of its data. We can then mark
	# buffers dirty for bgwriter without requiring WAL flush and thus preventing
	# opportunistic write combining.
	$node->safe_psql(
		'postgres', qq(
	CREATE TABLE wc_bgwriter (id int, payload text);
	INSERT INTO wc_bgwriter SELECT g, repeat('y', 200) FROM generate_series(1, 1000) AS g;
	SELECT flush_rel_buffers('wc_bgwriter'::regclass);
	));

	####
	# Test one big combined write
	####

	# Mark blocks 0-5 dirty
	dirty_blocks($psql, 'wc_bgwriter', '0,1,2,3,4,5');
	assert_blocks_dirty($node, 'wc_bgwriter', '0,1,2,3,4,5', 't',
		'contiguous buffers are dirty before bgwriter cleaner');

	flush_and_reset_io_stats($node, $psql);
	run_bgwriter_cleaner($psql);
	# Should have written one big write
	assert_writes($node, 'contiguous bgwriter cleaner', 'client backend',
		'normal', 1, 6 * $block_size);
	# None of those blocks should still be dirty
	assert_any_blocks_dirty($node, 'wc_bgwriter', '0,1,2,3,4,5', 'f',
		'bgwriter cleaner wrote contiguous dirty buffers');

	####
	# Test multiple single block writes when interspersed blocks are not in
	# shared buffers
	####

	# Evict every other block and mark the other blocks dirty
	$psql->query_safe(
		"SELECT invalidate_rel_blocks('wc_bgwriter', ARRAY[1,3,5])");
	dirty_blocks($psql, 'wc_bgwriter', '0,2,4');
	flush_and_reset_io_stats($node, $psql);

	run_bgwriter_cleaner($psql);
	# The three blocks that were dirty should have been written out in three
	# writes.
	assert_writes($node, 'nonresident gaps bgwriter cleaner', 'client backend',
		'normal', 3,
		3 * $block_size);
	# And none of them should be dirty anymore
	assert_any_blocks_dirty($node, 'wc_bgwriter', '0,2,4', 'f',
		'bgwriter cleaner wrote dirty buffers separated by nonresident gaps');

	####
	# Test two combined writes split around a pinned buffer. This covers all
	# code eagerly flushing non-pinned buffers, not just bgwriter.
	####

	# Make sure first six blocks are all read in and marked dirty
	dirty_blocks($psql, 'wc_bgwriter', '0,1,2,3,4,5');
	flush_and_reset_io_stats($node, $psql);

	# Do this in a transaction so that we can hold the buffer pin across
	# multiple SQL statements by transferring ownership to the top transaction
	# resource owner.
	$psql->query_safe("BEGIN");
	# Pin a block in the middle of the blocks
	my $pinned_buf = $psql->query_safe(
		"SELECT pin_rel_block('wc_bgwriter', 3)");

	run_bgwriter_cleaner($psql);
	# All the blocks should be in clean buffers except block 3 which was pinned
	# and should still be marked dirty.
	assert_any_blocks_dirty($node, 'wc_bgwriter', '0,1,2,4,5', 'f',
		'bgwriter cleaner wrote buffers around pinned gap');
	assert_blocks_dirty($node, 'wc_bgwriter', '3', 't',
		'bgwriter cleaner skipped pinned buffer');
	$psql->query_safe("SELECT release_buffer($pinned_buf)");
	$psql->query_safe("COMMIT");
	$psql->query_safe('SELECT pg_stat_force_next_flush()');
	# Should have written out blocks 0,1,2 in one write and blocks 4 and 5 in
	# another, totaling two writes.
	assert_writes($node, 'pinned gap bgwriter cleaner', 'client backend',
		'normal', 2, 5 * $block_size);

	$psql->quit();
}
