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
