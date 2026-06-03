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
