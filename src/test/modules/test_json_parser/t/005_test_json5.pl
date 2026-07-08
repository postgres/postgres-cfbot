
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test JSON5 support in the incremental JSON parser.  Each feature
# fixture must be accepted in --json5 mode, whole-input and
# byte-at-a-time, with identical semantic output, and rejected without
# --json5.  The non-incremental parser is exercised by SQL-level
# regression tests (see src/test/regress/sql/json5.sql).

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use File::Temp qw(tempfile);

my $dir = PostgreSQL::Test::Utils::tempdir;

my @exes = (
	[ "test_json_parser_incremental", ],
	[ "test_json_parser_incremental", "-o", ],
	[ "test_json_parser_incremental_shlib", ],
	[ "test_json_parser_incremental_shlib", "-o", ]);

# One entry per feature: fixture basename plus, where the failing token
# is predictable, a regex for the error reported without --json5.
my @features = (
	{
		name => 'comments',
		file => 'json5_comments',
		error => qr/Token "\/" is invalid/,
	},
	{ name => 'trailing commas', file => 'json5_trailing_commas' },
	{ name => 'unquoted keys', file => 'json5_keys' },
	{ name => 'single-quoted strings', file => 'json5_strings' },
	{ name => 'numbers', file => 'json5_numbers' },);

# Inputs that stay invalid even in json5 mode.
my @json5_invalid = (
	[ 'doubled trailing comma', '[1,,]' ],
	[ 'digit-led key', '{ 1a: 1 }' ],
	[ 'identifier value in object', '{ a: b }' ],
	[ 'identifier value in array', '[a]' ],
	[ 'bare identifier', 'undefined' ],
	[ 'dollar after number', '2$' ],
	[ 'dollar after number in array', '[25$]' ],
	[ 'bare 0x', '0x' ],
	[ 'bare dot', '.' ],
	[ 'missing value', '{"a":}' ],
	[ 'missing colon and value', '{"a"}' ],
	[ 'missing value after unquoted key', '{a:}' ],
	[ 'reserved word key without value', '{true}' ],
	[ 'missing colon and value after second key', '{"a":1,"b"}' ],
	[ 'unterminated block comment after value', '1 /*' ],
	[ 'unterminated block comment in array', '[1 /*' ],
	[ 'unterminated block comment with content', '1 /*/ 2' ],
	[ 'garbage after cr-terminated line comment', "1 // c\rx" ],
	[ 'signed infinity key', '{-Infinity: 1}' ],
	[ 'signed infinity with trailing junk', '-Infinityz' ],
	[ 'signed infinity with trailing dollar', '-Infinity$' ],
	[ 'signed nan with trailing junk', '+NaN5' ],);

# Valid json5 whose parse must not depend on where chunk boundaries
# fall (each caught a real bug: comment resume state, '$' in the
# partial-token continuation, identifier handling of Infinity/NaN).
my @json5_chunk_valid = (
	[ 'block comment containing /*-like text', '/*/ */ 1' ],
	[ 'dollar inside unquoted key', '{ab$c: 1}' ],
	[ 'Infinity as unquoted key', '{Infinity: 1}' ],
	[ 'NaN as unquoted key', '{NaN: 1}' ],
	[ 'negative NaN', '-NaN' ],
	[ 'positive NaN', '+NaN' ],
	[ 'cr-terminated line comment', "[1, // c\r2\n]" ],);

# Number extensions, each individually rejected without --json5.
my @json_invalid_numbers = (
	[ 'hex', '0x1F' ],
	[ 'leading dot', '.5' ],
	[ 'trailing dot', '5.' ],
	[ 'plus sign', '+42' ],
	[ 'infinity', 'Infinity' ],
	[ 'negative infinity', '-Infinity' ],
	[ 'positive infinity', '+Infinity' ],
	[ 'nan', 'NaN' ],);

# Write $content to a temp file and return the file name.
sub inline_file
{
	my ($content) = @_;
	my ($fh, $fname) = tempfile(DIR => $dir);
	print $fh $content;
	close($fh);
	return $fname;
}

# Parse $file with --json5 and compare the semantic output against
# $expected, whole-input and one byte at a time.
sub check_accepted
{
	my ($exe, $file, $expected, $label) = @_;

	foreach my $chunk (undef, 1)
	{
		my $mode = defined $chunk ? 'byte-at-a-time' : 'whole input';
		my @cmd = (@$exe, "-s", "--json5");
		push(@cmd, "-c", $chunk) if defined $chunk;

		my ($stdout, $stderr) = run_command([ @cmd, $file ]);

		is($stderr, "", "$label ($mode): no error output");

		my ($fh, $fname) = tempfile(DIR => $dir);
		print $fh $stdout, "\n";
		close($fh);

		my @diffopts = ("-u");
		push(@diffopts, "--strip-trailing-cr") if $windows_os;
		($stdout, $stderr) =
		  run_command([ "diff", @diffopts, $fname, $expected ]);

		is($stdout, "", "$label ($mode): no output diff");
		is($stderr, "", "$label ($mode): no diff error");
	}
}

# Check that parsing $file fails, matching $error on stderr if given.
sub check_rejected
{
	my ($exe, $file, $label, $error, @flags) = @_;

	my ($stdout, $stderr) = run_command([ @$exe, "-s", @flags, $file ]);

	unlike($stdout, qr/SUCCESS/, "$label: parsing fails");
	if (defined $error)
	{
		like($stderr, $error, "$label: correct error output");
	}
	else
	{
		isnt($stderr, "", "$label: error output");
	}
}

foreach my $exe (@exes)
{
	note "testing executable @$exe";

	foreach my $f (@features)
	{
		my $file = "$FindBin::RealBin/../$f->{file}.json5";
		my $expected = "$FindBin::RealBin/../$f->{file}.out";

		check_accepted($exe, $file, $expected, "json5 $f->{name}");
		check_rejected($exe, $file, "non-json5 mode: $f->{name}",
			$f->{error});
	}

	foreach my $inv (@json5_invalid)
	{
		my ($label, $content) = @$inv;
		my $fname = inline_file($content);

		for my $size (64, 1)
		{
			check_rejected($exe, $fname,
				"json5 mode, chunk size $size: $label",
				undef, "--json5", "-c", $size);
		}
	}

	foreach my $v (@json5_chunk_valid)
	{
		my ($label, $content) = @$v;
		my $fname = inline_file($content);

		my ($whole, $whole_err) =
		  run_command([ @$exe, "-s", "--json5", $fname ]);

		is($whole_err, "", "json5 mode, whole input: $label: no error");

		for my $size (3, 1)
		{
			my ($stdout, $stderr) =
			  run_command([ @$exe, "-s", "--json5", "-c", $size, $fname ]);

			is($stderr, "",
				"json5 mode, chunk size $size: $label: no error");
			is($stdout, $whole,
				"json5 mode, chunk size $size: $label: matches whole input");
		}
	}

	# Specifically exercise a hex number split in the middle of its
	# digit run (0x1|F) and a leading-dot number split right after the
	# dot (.|5), at the chunk size that forces exactly that split.
	my ($stdout, $stderr) =
	  run_command([ @$exe, "-s", "--json5", "-c", 3, inline_file("0x1F") ]);

	is($stdout, "0x1F", "json5 mode: 0x1|F split reassembles");
	is($stderr, "", "json5 mode: 0x1|F split: no error output");

	($stdout, $stderr) =
	  run_command([ @$exe, "-s", "--json5", "-c", 1, inline_file(".5") ]);

	is($stdout, ".5", "json5 mode: .|5 split reassembles");
	is($stderr, "", "json5 mode: .|5 split: no error output");

	foreach my $inv (@json_invalid_numbers)
	{
		my ($label, $content) = @$inv;

		check_rejected($exe, inline_file($content),
			"non-json5 mode: $label form");
	}
}

done_testing();
