# Copyright (c) 2026, PostgreSQL Global Development Group

# Test gzip reading in pg_restore. With zlib 1.3.2, bug #19754 rejects a valid
# file when both sizes end at buffer boundaries, but accepts a truncated file.

use strict;
use warnings FATAL => 'all';

use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $supports_gzip = check_pg_config("#define HAVE_LIBZ 1");

plan skip_all => 'gzip compression not supported by this build'
	if !$supports_gzip;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Create enough data to fill several buffers. The row with id = 0 lets us add
# bytes later to make the uncompressed size end at a buffer boundary.
$node->safe_psql('postgres', q{
	CREATE TABLE gzip_boundary_test (id int, v text);
	INSERT INTO gzip_boundary_test
		SELECT i, md5(i::text) FROM generate_series(1, 20000) AS i;
	INSERT INTO gzip_boundary_test VALUES (0, '');
});

# Make an initial dump to find the uncompressed size of the table data.
my $dumpdir = "$tempdir/gzip_boundary_dump";
my $restore_file = "$tempdir/gzip_boundary_restore.sql";
$node->command_ok(
	[
		'pg_dump', '--no-sync', '--format' => 'directory',
		'--compress' => 'gzip:1', '--file' => $dumpdir,
		'--table' => 'gzip_boundary_test', 'postgres'
	],
	'create a gzip directory dump');

my ($datafile) = glob("$dumpdir/*.dat.gz");
die "could not find gzip data file in $dumpdir" if !defined($datafile);

# Make the uncompressed data size a multiple of pg_restore's I/O buffer size.
# The last four gzip bytes store the uncompressed size as a little-endian
# integer, modulo 2^32. The data here is small enough to use that size directly.
my $input_buffer_size = 128 * 1024;
open my $fh, '<:raw', $datafile or die "could not open $datafile: $!";
my $gzip_data = do { local $/; <$fh> };
close $fh or die "could not close $datafile: $!";
my $uncompressed_size = unpack('V', substr($gzip_data, -4));
# Add only enough bytes to reach the next boundary. If already there, add none.
my $padding = ($input_buffer_size - $uncompressed_size % $input_buffer_size)
	% $input_buffer_size;
# Each 'y' adds one byte to the dumped data without needing COPY escaping.
$node->safe_psql('postgres',
	"UPDATE gzip_boundary_test SET v = repeat('y', $padding) WHERE id = 0");

# Replace the first dump with a dump containing the added bytes.
rmtree($dumpdir);
$node->command_ok(
	[
		'pg_dump', '--no-sync', '--format' => 'directory',
		'--compress' => 'gzip:1', '--file' => $dumpdir,
		'--table' => 'gzip_boundary_test', 'postgres'
	],
	'redump with uncompressed size on the buffer boundary');

($datafile) = glob("$dumpdir/*.dat.gz");
open $fh, '<:raw', $datafile or die "could not open $datafile: $!";
$gzip_data = do { local $/; <$fh> };
close $fh or die "could not close $datafile: $!";
$uncompressed_size = unpack('V', substr($gzip_data, -4));
is($uncompressed_size % $input_buffer_size, 0,
	'uncompressed size is a multiple of 128 kB');

# Make the compressed size a multiple of zlib's 8192-byte input buffer size.
# Add extra bytes to the gzip header so the table data and trailer stay the same.
my $compressed_input_buffer_size = 8192;
# Include the two-byte length field when calculating how many bytes to add.
my $extra_size = ($compressed_input_buffer_size -
	(length($gzip_data) + 2) % $compressed_input_buffer_size) %
	$compressed_input_buffer_size;
# Set the FEXTRA flag in the fourth header byte to mark the extra field.
substr($gzip_data, 3, 1) = chr(ord(substr($gzip_data, 3, 1)) | 4);
# After the ten-byte header, insert the length followed by that many zero bytes.
$gzip_data = substr($gzip_data, 0, 10)
	. pack('v', $extra_size) . ("\0" x $extra_size)
	. substr($gzip_data, 10);
is(length($gzip_data) % $compressed_input_buffer_size, 0,
	'compressed size is a multiple of 8192 bytes');

open $fh, '>:raw', $datafile or die "could not open $datafile: $!";
print {$fh} $gzip_data or die "could not write $datafile: $!";
close $fh or die "could not close $datafile: $!";

# Write SQL to a file rather than restoring into a database, so these tests
# check the gzip reader without depending on COPY accepting the output.
$node->command_ok(
	[
		'pg_restore', '--file' => $restore_file, $dumpdir
	],
	'restore the intact gzip file whose sizes are on both buffer boundaries');

# COPY rows start with an integer and a tab. Check that all rows were written.
my $restored = slurp_file($restore_file);
my @rows = $restored =~ /^\d+\t/gm;
is(scalar(@rows), 20001, 'all table rows were decompressed');

# Cut the file in half, as in the bug report. Some data can still be read, but
# pg_restore must fail because the gzip stream is incomplete.
my $truncated_size = int(length($gzip_data) / 2);
open $fh, '+<:raw', $datafile or die "could not open $datafile: $!";
truncate($fh, $truncated_size) or die "could not truncate $datafile: $!";
close $fh or die "could not close $datafile: $!";

$node->command_fails(
	[
		'pg_restore', '--file' => "$tempdir/gzip_boundary_truncated.sql",
		$dumpdir
	],
	'fail to restore the truncated gzip data file');

done_testing();
