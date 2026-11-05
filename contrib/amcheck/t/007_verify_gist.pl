# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that gist_index_check() reports corruption it is supposed to find.
# The regression tests only run it on healthy indexes, which cannot tell a
# working checker from one that never reports anything.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node;
my $blksize;

$node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;
$blksize = int($node->safe_psql('postgres', 'SHOW block_size;'));
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));

inconsistent_parent_key_test();
missing_heap_tuple_test();
invalid_link_test('rightlink');
invalid_link_test('downlink');
invalid_link_test('root');

$node->stop;
done_testing();

# Build a point index with an internal root page.  All points are in
# [0, 1000] except the ones given, so their coordinates are easy to find
# in the index file.
sub create_point_index
{
	my ($relname, $indexname, @extra_points) = @_;

	my $extra = join('',
		map { "INSERT INTO $relname VALUES (point($_, $_));\n" }
		  @extra_points);

	$node->safe_psql(
		'postgres', qq(
		DROP TABLE IF EXISTS $relname;
		CREATE TABLE $relname (p point);
		INSERT INTO $relname
		  SELECT point(random() * 1000, random() * 1000)
		  FROM generate_series(1, 5000);
		$extra
		CREATE INDEX $indexname ON $relname USING gist (p);
	));

	# The corruption below relies on the root page being an internal page.
	my $pages = $node->safe_psql('postgres',
		qq(SELECT pg_relation_size('$indexname') / $blksize));
	cmp_ok($pages, '>', 2, "$indexname has more than one level");

	# A healthy index must pass both kinds of check.
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		qq(SELECT gist_index_check('$indexname', true)));
	is($ret, 0, "healthy $indexname passes gist_index_check");
	is($stderr, '', "healthy $indexname reports nothing");
}

# Shrink a downlink key in the root page, so that it no longer covers the
# keys on the child page.
sub inconsistent_parent_key_test
{
	my $relname = 'gist_parent';
	my $indexname = 'gist_parent_idx';

	create_point_index($relname, $indexname, 99999);
	my $relpath = relation_filepath($indexname);

	$node->stop;
	my $n = float8_replace_blocks($relpath, 99999, 1, 0, 0);
	cmp_ok($n, '>', 0, 'corrupted downlink key in root page');
	$node->start;

	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		qq(SELECT gist_index_check('$indexname', false)));
	like(
		$stderr,
		qr/index "$indexname" has inconsistent records on page \d+ offset \d+/,
		'inconsistent downlink key is reported');
}

# Change one leaf key to a value still covered by its parent.  The tree
# stays consistent, so only heapallindexed can notice the heap tuple that
# has no index entry.
sub missing_heap_tuple_test
{
	my $relname = 'gist_leaf';
	my $indexname = 'gist_leaf_idx';

	create_point_index($relname, $indexname, 77777, 99999);
	my $relpath = relation_filepath($indexname);
	my $nblocks = (-s $relpath) / $blksize;

	$node->stop;
	# Skip the root page, only the leaf copy of the key is changed.
	my $n = float8_replace_blocks($relpath, 77777, 77776.5, 1, $nblocks - 1);
	is($n, 4, 'changed both corners of one leaf key');
	$node->start;

	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		qq(SELECT gist_index_check('$indexname', false)));
	is($stderr, '', 'structure check alone does not notice a changed leaf key');

	($ret, $stdout, $stderr) = $node->psql('postgres',
		qq(SELECT gist_index_check('$indexname', true)));
	like(
		$stderr,
		qr/heap tuple \(\d+,\d+\) from table "$relname" lacks matching index tuple within index "$indexname"/,
		'heapallindexed reports the heap tuple without index tuple');
}

# Corrupt a link that the check follows.  InvalidBlockNumber is P_NEW, and
# reading it would extend the index, so check that the size does not change.
sub invalid_link_test
{
	my ($kind) = @_;
	my $relname = "gist_$kind";
	my $indexname = "gist_${kind}_idx";

	create_point_index($relname, $indexname, 99999);
	my $relpath = relation_filepath($indexname);
	my $size = -s $relpath;

	$node->stop;
	if ($kind eq 'rightlink')
	{
		# Mark a non-root page as split, with no right sibling.
		modify_block($relpath, 1,
			sub { set_opaque($_[0], 0xFFFFFFFF, 1 << 3) });
	}
	elsif ($kind eq 'root')
	{
		# Mark the root page as split, with a right sibling that exists.
		modify_block($relpath, 0, sub { set_opaque($_[0], 1, 1 << 3) });
	}
	else
	{
		# The root downlink whose key covers the extra point.  Its t_tid
		# is just before the key.
		modify_block(
			$relpath, 0,
			sub {
				my $pos = index($_[0], pack('dd', 99999, 99999));
				die "downlink key not found" if $pos < 8;
				substr($_[0], $pos - 8, 4) = pack('SS', 0xFFFF, 0xFFFF);
			});
	}
	$node->start;

	my %expected = (
		rightlink => qr/index "$indexname" has page 1 marked as split without right sibling/,
		downlink => qr/index "$indexname" has invalid downlink on page 0 offset \d+/,
		root => qr/index "$indexname" has root page marked as split/);
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		qq(SELECT gist_index_check('$indexname', false)));
	like($stderr, $expected{$kind}, "invalid $kind is reported");
	is(-s $relpath, $size, "check with invalid $kind does not extend the index");
}

# Set the right link and add flags in the GiST page opaque data.
sub set_opaque
{
	my ($rightlink, $flags) = @_[1, 2];
	my $special = unpack('S', substr($_[0], 16, 2));
	my $oldflags = unpack('S', substr($_[0], $special + 12, 2));

	substr($_[0], $special + 8, 6) = pack('LS', $rightlink, $oldflags | $flags);
}

# Read one block, let the callback change it in place, and write it back.
sub modify_block
{
	my ($filename, $blkno, $callback) = @_;
	my $buffer;

	open(my $fh, '+<', $filename) or BAIL_OUT("open failed: $!");
	binmode $fh;
	sysseek($fh, $blkno * $blksize, 0) or BAIL_OUT("seek failed: $!");
	sysread($fh, $buffer, $blksize) == $blksize
	  or BAIL_OUT("read failed: $!");
	$callback->($buffer);
	sysseek($fh, $blkno * $blksize, 0) or BAIL_OUT("seek failed: $!");
	syswrite($fh, $buffer) == $blksize or BAIL_OUT("write failed: $!");
	close($fh) or BAIL_OUT("close failed: $!");
}

sub relation_filepath
{
	my ($relname) = @_;

	my $pgdata = $node->data_dir;
	my $rel = $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$relname')));
	die "path not found for relation $relname" unless defined $rel;
	return "$pgdata/$rel";
}

# Replace every float8 value 'find' with 'replace' in blocks first..last of
# the file.  Values are packed in native byte order, as stored on disk.
# Returns the number of replacements.
sub float8_replace_blocks
{
	my ($filename, $find, $replace, $first, $last) = @_;
	my $pattern = quotemeta(pack('d', $find));
	my $new = pack('d', $replace);
	my $count = 0;

	open(my $fh, '+<', $filename) or BAIL_OUT("open failed: $!");
	binmode $fh;

	for my $blkno ($first .. $last)
	{
		my $offset = $blkno * $blksize;
		my $buffer;

		sysseek($fh, $offset, 0) or BAIL_OUT("seek failed: $!");
		sysread($fh, $buffer, $blksize) == $blksize
		  or BAIL_OUT("read failed: $!");

		my $n = ($buffer =~ s/$pattern/$new/g);
		next unless $n;
		$count += $n;

		sysseek($fh, $offset, 0) or BAIL_OUT("seek failed: $!");
		syswrite($fh, $buffer) == $blksize or BAIL_OUT("write failed: $!");
	}

	close($fh) or BAIL_OUT("close failed: $!");
	return $count;
}
