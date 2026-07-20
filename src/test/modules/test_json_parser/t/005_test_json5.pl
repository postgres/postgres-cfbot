
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test JSON5 support in the standalone (recursive descent) JSON parser.
# Each feature fixture must be accepted in --json5 mode with the
# expected semantic output, and rejected without --json5.  The
# incremental parser does not support JSON5.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use File::Temp qw(tempfile);

my $dir = PostgreSQL::Test::Utils::tempdir;

my @exes = (
	[ "test_json_parser_standalone", ],
	[ "test_json_parser_standalone", "-o", ],
	[ "test_json_parser_standalone_shlib", ],
	[ "test_json_parser_standalone_shlib", "-o", ]);

# Inline json5 inputs with their expected semantic output, or undef for
# inputs that must be rejected in json5 mode.  Non-ASCII input is given
# as UTF-8 bytes.
my @json5_inline_cases = (
	[ 'leading no-break space', "\xc2\xa0[1]", "[\n1\n]" ],
	[ 'leading byte order mark', "\xef\xbb\xbf[1]", "[\n1\n]" ],
	[ 'vertical tab and form feed', "[1,\x0b2,\f3]", "[\n1,\n2,\n3\n]" ],
	[ 'ideographic space', "[1,\xe3\x80\x80 2]", "[\n1,\n2\n]" ],
	[ 'line comment ended by end of input', "1 // c", "1" ],
	[
		'comment markers inside a string',
		"[\"a /* b */ c // d\"]", "[\n\"a /* b */ c // d\"\n]"
	],
	[ 'block comment only', "/* c */", undef ],
	[ 'line comment only', "// c", undef ],
	[
		'line comment ended by U+2028',
		"[1, // c\xe2\x80\xa8 2]", "[\n1,\n2\n]"
	],
	[
		'line comment ended by U+2029',
		"[1, // c\xe2\x80\xa9 2]", "[\n1,\n2\n]"
	],
	[ 'non-space symbol outside strings', "[1,\xe2\x80\xa6 2]", undef ],
	[ 'invalid UTF-8 outside strings', "[1,\xc2 2]", undef ],
	[
		'no-break space around unquoted key',
		"{\xc2\xa0a\xc2\xa0: 1}", "{\n\"a\": 1\n}"
	],
	[ 'non-ASCII letter in key', "{\xc3\xa9t\xc3\xa9: 1}", "{\n\"\xc3\xa9t\xc3\xa9\": 1\n}" ],
	[ 'combining mark after first key char', "{a\xcc\x81: 1}", "{\n\"a\xcc\x81\": 1\n}" ],
	[ 'combining mark as first key char', "{\xcc\x81a: 1}", undef ],
	[ 'zero width non-joiner in key', "{a\xe2\x80\x8cb: 1}", "{\n\"a\xe2\x80\x8cb\": 1\n}" ],
	[ 'non-identifier symbol in key', "{a\xe2\x80\xa6b: 1}", undef ],
	[ 'escaped key characters', "{\\u0061b\\u0063: 1}", "{\n\"abc\": 1\n}" ],
	[ 'escaped non-ASCII key character', "{\\u00e9: 1}", "{\n\"\xc3\xa9\": 1\n}" ],
	[ 'escaped space in key', "{a\\u0020b: 1}", undef ],
	[ 'malformed escape in key', "{a\\u00zz: 1}", undef ],
	[ 'escaped surrogate pair in key', "{\\ud83d\\ude00: 1}", undef ],
	[ 'keyword continued by escape', "{true\\u0061: 1}", "{\n\"truea\": 1\n}" ],
	[ 'escaped keyword as key', "{\\u0074rue: 1}", "{\n\"true\": 1\n}" ],
	[ 'escaped keyword as value', "[\\u0074rue]", undef ],
	[ 'keyword followed by no-break space', "[true\xc2\xa0]", "[\ntrue\n]" ],);

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
	{ name => 'single-quoted strings', file => 'json5_strings' },);

# Inputs that stay invalid even in json5 mode.
my @json5_invalid = (
	[ 'doubled trailing comma', '[1,,]' ],
	[ 'doubled trailing comma in object', '{"a":1,,}' ],
	[ 'leading comma', '[,1]' ],
	[ 'leading comma in object', '{,"a":1}' ],
	[ 'lone comma', '[,]' ],
	[ 'lone comma in object', '{,}' ],
	[ 'missing comma', '[true false]' ],
	[ 'missing comma in object', '{"a":1 "b":2}' ],
	[ 'digit-led key', '{ 1a: 1 }' ],
	[ 'identifier value in object', '{ a: b }' ],
	[ 'identifier value in array', '[a]' ],
	[ 'bare identifier', 'undefined' ],
	[ 'dollar after number', '2$' ],
	[ 'dollar after number in array', '[25$]' ],
	[ 'digit as first key', '{1: 1}' ],
	[ 'hyphen inside unquoted key', '{multi-word: 1}' ],);

# Valid json5 corner cases not covered by the feature fixtures.
my @json5_misc_valid = (
	[ 'reserved word as first key', '{true: 1, a: 2}' ],);

# Write $content to a temp file and return the file name.  The file is
# written in binary mode, so that CR and LF in $content reach the parser
# as they are.
sub inline_file
{
	my ($content) = @_;
	my ($fh, $fname) = tempfile(DIR => $dir);
	binmode($fh);
	print $fh $content;
	close($fh);
	return $fname;
}

# Run the parser executable @$exe with @args and return its output, with
# the CRLF line ends that printf() produces on Windows normalized to LF.
sub run_parser
{
	my ($exe, @args) = @_;
	my ($stdout, $stderr) = run_command([ @$exe, @args ]);

	$stdout =~ s/\r\n/\n/g if $windows_os;
	return ($stdout, $stderr);
}

# Parse $file with --json5 and compare the semantic output against
# $expected.
sub check_accepted
{
	my ($exe, $file, $expected, $label) = @_;

	my ($stdout, $stderr) = run_parser($exe, "-s", "--json5", $file);

	is($stderr, "", "$label: no error output");

	my ($fh, $fname) = tempfile(DIR => $dir);
	print $fh $stdout, "\n";
	close($fh);

	my @diffopts = ("-u");
	push(@diffopts, "--strip-trailing-cr") if $windows_os;
	($stdout, $stderr) =
	  run_command([ "diff", @diffopts, $fname, $expected ]);

	is($stdout, "", "$label: no output diff");
	is($stderr, "", "$label: no diff error");
}

# Check that parsing $file fails, matching $error on stderr if given.
sub check_rejected
{
	my ($exe, $file, $label, $error, @flags) = @_;

	my ($stdout, $stderr) = run_parser($exe, "-s", @flags, $file);

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

	foreach my $c (@json5_inline_cases)
	{
		my ($label, $content, $expected) = @$c;
		my $fname = inline_file($content);

		if (defined $expected)
		{
			my ($stdout, $stderr) =
			  run_parser($exe, "-s", "--json5", $fname);

			is($stdout, $expected, "json5 mode: $label: output");
			is($stderr, "", "json5 mode: $label: no error output");
		}
		else
		{
			check_rejected($exe, $fname, "json5 mode: $label", undef,
				"--json5");
		}
	}

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

		check_rejected($exe, $fname, "json5 mode: $label", undef, "--json5");
	}

	foreach my $v (@json5_misc_valid)
	{
		my ($label, $content) = @$v;
		my $fname = inline_file($content);

		my ($stdout, $stderr) = run_parser($exe, "--json5", $fname);

		like($stdout, qr/SUCCESS/, "json5 mode: $label: parse succeeds");
		is($stderr, "", "json5 mode: $label: no error output");
	}
}

done_testing();
