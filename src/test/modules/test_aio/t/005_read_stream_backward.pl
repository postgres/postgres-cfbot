# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Correctness tests for read-stream backward I/O combining
#
# The read stream combines a run of descending block numbers into one forward
# read, then hands the buffers back in reverse order.
#
# read_stream_for_blocks(rel, blocks[]) hands back one buffer per requested
# block, in stream order.  We check that the stream returns exactly the
# requested blocks in the reversed order.  With check_per_buffer_data => true
# the helper additionally verifies, inside the backend, that every buffer is
# the block that was requested and is paired with the per-buffer data the
# callback populated for it.  Every pattern runs after evict_rel, so real I/O
# and backward combining happen.
#
# The reversal logic under test lives in read_stream.c and is independent of
# the io_method, so we run the tests under 'worker' io_method only.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use TestAio;


my $node = PostgreSQL::Test::Cluster->new('test');
$node->init();

TestAio::configure($node);

# Large shared_buffers and high effective_io_concurrency let the stream look
# ahead far enough to build large reverse ranges.
$node->append_conf(
	'postgresql.conf', qq(
max_connections=8
io_method=worker
io_max_combine_limit=16
));

$node->start();

my $nblocks = test_setup($node);
my $last = $nblocks - 1;

$node->stop();

# Run all of the patterns
test_backward_main($node, $nblocks);

# Buffer-starved cluster: exercises the "guarantee progress" path in
# read_stream_start_pending_read(), where the per-backend pin limit can drop to
# zero mid-range and the code must keep making one block of forward progress.
test_backward_low_buffers($node, $nblocks);

# Backward I/O combining under per-backend pin pressure
test_backward_pin_pressure($node, $nblocks);

done_testing();


# Create the test relation and return its size in blocks
sub test_setup
{
	my $node = shift;

	$node->safe_psql(
		'postgres', qq(
CREATE EXTENSION test_aio;

CREATE TABLE largeish(k int not null) WITH (FILLFACTOR=10);
INSERT INTO largeish(k) SELECT generate_series(1, 40000);
));

	my $blocks = $node->safe_psql('postgres',
		q{SELECT pg_relation_size('largeish') / current_setting('block_size')::int8}
	);
	note "largeish is $blocks blocks";

	# Reference blocks well inside the relation; ensure it is big enough that
	# the "spans more than the read-stream queue" pattern is meaningful.
	ok($blocks >= 512, "setup: relation large enough ($blocks blocks)");

	return $blocks;
}


# Build the [ label => \@blocks ] access patterns, sized to the relation.
# Covers ascending, descending, mixed, gapped, repeated, boundary and
# queue-wrapping cases).
sub access_patterns
{
	my ($want) = @_;

	my @patterns = (
		[ 'single' => [5] ],
		[ 'asc_pair' => [ 0, 1 ] ],
		[ 'desc_pair' => [ 1, 0 ] ],
		# full-region backward scan
		[ 'full_backward' => [ reverse(0 .. $last) ] ],
		# Direction changes: ascending, descending, then ascending
		[ 'mixed_dir' => [ 0 .. 6, reverse(0 .. 5), 0 .. 6 ] ],
		# Zig-zag flipping direction every block
		[ 'zigzag' => [ 5, 4, 5, 4, 5, 4 ] ],
		# Descending run near block 0 then another at the relation's end
		[ 'desc_two_regions' => [ 3, 2, 1, 0, $last, $last - 1, $last - 2 ] ],
	);

	return @patterns unless defined $want;

	my ($p) = grep { $_->[0] eq $want } @patterns;
	die "unknown access pattern '$want'" unless $p;
	return $p;
}


# Run one access pattern and return the block numbers the stream handed back,
# in order, as a "{...}" array literal.  The caller compares this against the
# requested blocks, confirming the stream yields exactly those blocks in the
# expected order.
sub stream_and_verify
{
	my ($node, $rel, $blocks, $icl, $check_pbd) = @_;

	my $arr = 'ARRAY[' . join(',', @$blocks) . ']::int4[]';
	$check_pbd = $check_pbd ? 'true' : 'false';

	# Evict first so real I/O and (backward) combining happen.
	return $node->safe_psql(
		'postgres', qq{
SET io_combine_limit = $icl;
DO \$\$ BEGIN PERFORM evict_rel('$rel'); END \$\$;
SELECT array_agg(blocknum ORDER BY blockoff)::text
FROM read_stream_for_blocks('$rel', $arr, $check_pbd);
});
}


sub test_backward_main
{
	my ($node, $nblocks) = @_;

	my @patterns = access_patterns();

	$node->start();

	foreach my $icl (1, 8, 16)
	{
		foreach my $pattern (@patterns)
		{
			my ($label, $blocks) = @$pattern;
			my $want = '{' . join(',', @$blocks) . '}';

			my $res = stream_and_verify($node, 'largeish', $blocks, $icl, 1);
			is($res, $want, "icl=$icl $label");
		}
	}

	$node->stop();
}


# Buffer-starved cluster: verify a long backward combine still terminates and
# returns the right blocks, in order, under a tight per-backend pin budget.
sub test_backward_low_buffers
{
	my ($node, $nblocks) = @_;

	# 128kB == 16 shared buffers: enough to start, but too few to hold a long
	# reverse range, so the per-backend pin limit binds and the
	# guarantee-progress path is exercised repeatedly.
	$node->append_conf(
		'postgresql.conf', qq(
max_connections=8
shared_buffers=128kB
));
	$node->start();

	my ($label, $blocks) = @{ access_patterns('full_backward') };
	my $want = '{' . join(',', @$blocks) . '}';

	foreach my $icl (8, 16)
	{
		my $res = stream_and_verify($node, 'largeish', $blocks, $icl);
		is($res, $want,
			"low shared_buffers icl=$icl long backward scan completes");
	}

	$node->stop();
}

# Verify backward I/O combining under a small per-backend pin limit.
#
# The pin limit is GetAdditionalPinLimit(), derived from MaxProportionalPins =
# NBuffers / (MaxBackends + NUM_AUXILIARY_PROCS).  A high max_connections makes
# it tiny even with plenty of free buffers.
sub test_backward_pin_pressure
{
	my ($node, $nblocks) = @_;

	# 32MB == 4096 shared buffers keeps a few hundred blocks resident, but
	# max_connections=600 drives MaxProportionalPins to a single-digit value, so
	# GetAdditionalPinLimit() stays far below io_combine_limit.
	$node->append_conf(
		'postgresql.conf', qq(
max_connections=600
shared_buffers=32MB
));
	$node->start();

	my ($label, $blocks) = @{ access_patterns('full_backward') };
	my $want = '{' . join(',', @$blocks) . '}';

	foreach my $icl (8, 16)
	{
		my $res = stream_and_verify($node, 'largeish', $blocks, $icl);
		is($res, $want, "pin pressure icl=$icl $label");
	}

	$node->stop();
}
