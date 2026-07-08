
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
	},);

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

}

done_testing();
