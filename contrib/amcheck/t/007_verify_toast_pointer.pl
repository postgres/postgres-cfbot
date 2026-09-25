
# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that verify_heapam() reports TOAST pointers whose compression method
# is not stored in the form appropriate for it, without crashing, and that a
# well-formed pointer is not reported.
#
# A pointer is forged by rewriting bytes of a heap page on disk: the method
# bits of va_extinfo, and the vartag, which tells the plain form of a pointer
# from the long form that carries the method in a byte of its own.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE EXTENSION pageinspect));

# One row per case, each with one compressed external value: a low
# toast_tuple_target pushes the compressed values out of line.
my $nrows = 3;
$node->safe_psql(
	'postgres', qq(
	CREATE TABLE tp (id int, f text COMPRESSION pglz)
		WITH (autovacuum_enabled = false, toast_tuple_target = 128);
	INSERT INTO tp
		SELECT g, repeat('abcdefgh' || g, 2000) FROM generate_series(1, $nrows) g;
	CHECKPOINT;
));

# The values must be compressed and stored externally.
is( $node->safe_psql(
		'postgres', q(
	SELECT string_agg(pg_column_compression(f) || ':' ||
					  (pg_column_size(f) < octet_length(f)), ',' ORDER BY id)
		FROM tp)),
	join(',', ('pglz:true') x $nrows),
	'values are compressed with pglz');
is($node->safe_psql('postgres', q(SELECT * FROM verify_heapam('tp'))),
	'', 'no corruption before forging');

# Find each tuple's data in the page.  A tuple is (int4 id, text f), and f is
# an on-disk TOAST pointer in the plain form: one byte 0x01, one byte vartag,
# then va_rawsize (int4) and va_extinfo (uint32), unaligned.  So va_extinfo
# is at offset 4 + 2 + 4 of the tuple data.
my $relpath = $node->data_dir . '/'
  . $node->safe_psql('postgres', q(SELECT pg_relation_filepath('tp')));
my %extinfo_off;
for my $row (
	split /\n/,
	$node->safe_psql(
		'postgres', q(
	SELECT lp, lp_off + t_hoff, get_byte(t_data, 4), get_byte(t_data, 5)
		FROM heap_page_items(get_raw_page('tp', 0))
		ORDER BY lp)))
{
	my ($lp, $dataoff, $b4, $b5) = split /\|/, $row;
	is("$b4/$b5", "1/18", "tuple $lp holds a plain on-disk TOAST pointer");
	$extinfo_off{$lp} = $dataoff + 4 + 2 + 4;
}

$node->stop;
my $fh;
open($fh, '+<', $relpath) or BAIL_OUT("open failed: $!");
binmode $fh;

# Rewrite the two method bits (the top bits) of a va_extinfo
sub set_method_bits
{
	my ($off, $bits) = @_;
	my $code = (pack('L', 1) eq pack('V', 1)) ? 'V' : 'N';
	my $buf;

	sysseek($fh, $off, 0) or BAIL_OUT("sysseek failed: $!");
	sysread($fh, $buf, 4) == 4 or BAIL_OUT("sysread failed: $!");
	my $extinfo = (unpack($code, $buf) & 0x3FFFFFFF) | ($bits << 30);
	sysseek($fh, $off, 0) or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, pack($code, $extinfo)) == 4
	  or BAIL_OUT("syswrite failed: $!");
}

# Case 1: a plain pointer whose method bits say "long form"
set_method_bits($extinfo_off{1}, 3);

# Case 2: a plain pointer whose method bits name a method that needs the long
# form (2, zstd's ID once it exists, and in any case not pglz or lz4)
set_method_bits($extinfo_off{2}, 2);

# Row 3 is left alone.
close($fh) or BAIL_OUT("close failed: $!");
$node->start;

my $result = $node->safe_psql('postgres',
	q(SELECT offnum, msg FROM verify_heapam('tp') ORDER BY offnum, msg));
like(
	$result,
	qr/^1\|toast value \d+ has invalid compression method id 3$/m,
	'plain pointer with long-form method bits is reported');
like(
	$result,
	qr/^2\|toast value \d+ has invalid compression method id 2$/m,
	'plain pointer naming a long-form method is reported');
unlike($result, qr/^3\|/m, 'well-formed pointer is not reported');

# Reading the forged values fails cleanly instead of crashing.
for my $id (1, 2)
{
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		"SELECT pg_column_compression(f) FROM tp WHERE id = $id");
	like($stderr, qr/compressed data is corrupt/,
		"pg_column_compression() rejects forged pointer $id");
}
is($node->safe_psql('postgres', q(SELECT 1)), '1', 'server still up');

done_testing();
